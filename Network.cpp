#include "Network.hpp"
#include "common.h"

#include <unistd.h>
#include <fstream>
#include <thread>
#include <algorithm>

using namespace Network;

static State state = CONNECTING;
static bool quit = false;

static int ds_sock = -1;
static int udp_sock = -1;
static int input_sock = -1;
static struct sockaddr_in ds_addr;

static int connect_attempts = 0;

void Network::ConnectDS(const std::string host) {
    int ret;

    if (ds_sock < 0) {
        ds_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (ds_sock < 0) {
            NetworkError("Couldn't make socket");
            return;
        }
    }

    ds_addr = (struct sockaddr_in) {
        .sin_family = AF_INET,
        .sin_port = htons(8000),
        .sin_addr = {0},
        .sin_zero = {0},
    }; //TODO
    ret = inet_pton(ds_addr.sin_family, host.c_str(), &(ds_addr.sin_addr));
    if (ret <= 0) {
        NetworkErrorF("Address %s invalid - check your config file", host.c_str());
        if (ds_sock >= 0) {
            close(ds_sock);
            ds_sock = -1;
        }
        state = ERR_BAD_IP;
        return;
    }

    state = CONNECTING;
    ret = connect(ds_sock, (struct sockaddr*)&ds_addr, sizeof(ds_addr));
    if (ret < 0) {
        NetworkErrorF("Can't connect to DS (%s)", host.c_str());
        if (ds_sock >= 0) {
            close(ds_sock);
            ds_sock = -1;
        }
        return;
    }
}

void Network::ListenUDP() {
    int ret;

    if (udp_sock < 0) {
        udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    }

    struct sockaddr_in listen_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(8001),
        .sin_addr = {
            .s_addr = htonl(INADDR_ANY),
        },
        .sin_zero = {0},
    };
    ret = bind(udp_sock, (struct sockaddr*)&listen_addr, sizeof(listen_addr));
    if (ret < 0) {
        NetworkError("Can't bind to UDP");
        if (udp_sock >= 0) {
            close(udp_sock);
            udp_sock = -1;
        }
        return;
    }
}

typedef struct jpeg {
    uint8_t id;
    std::array<bool, 256> seq;
    uint8_t seq_last;
    bool pending;
    bool good;
    std::vector<uint8_t> data;
} jpeg;

static std::array<jpeg, 256> receivingFrames_top;
static std::array<jpeg, 256> receivingFrames_btm;

static void initJpeg(jpeg& jpeg) {
    jpeg.seq.fill(false);
    jpeg.seq_last = 254;
    jpeg.pending = false;
    jpeg.good = false;
    jpeg.data.reserve(30000);
    jpeg.data.clear();
}

static jpeg& getJpegForID(uint8_t id, int isTop) {
    auto& receivingFrames = (isTop) ? receivingFrames_top : receivingFrames_btm;
    auto& jpeg = receivingFrames[id];
    if (jpeg.good) {
        //this will never be hit for recent frames, only old (>127) ones
        initJpeg(jpeg);
    }
    jpeg.id = id; //TODO refactor this out
    return jpeg;
}

static uint8_t lastGoodID_top = 255;
static uint8_t lastGoodID_btm = 255;

static void checkGoodJpeg(jpeg& jpeg, int isTop) {
    auto& lastGoodID = (isTop) ? lastGoodID_top : lastGoodID_btm;
    auto& receivingFrames = (isTop) ? receivingFrames_top : receivingFrames_btm;
    if (jpeg.pending) {
        //check we actually got a full frame
        bool all_seqs = std::all_of(jpeg.seq.cbegin(),
            std::next(jpeg.seq.cbegin(), jpeg.seq_last),
            [](bool b){ return b; }
        );
        //all seqs OK, ready to display
        if (all_seqs) {
            //printf("frame %d OK\n", jpeg.id);
            jpeg.good = true;
            //only interested in data for frames after this one
            lastGoodID = jpeg.id;
            //let display know we're good
            state = CONNECTED_STREAMING;
            //hack: set the last few frames as good, too. Helps getJpegForID
            //clear out old state.
            //TODO: come up with a less flaky solution for this
            //did you know? C++'s % operator does not affect negative numbers.
            //nice OOB write, that one. adding 256 as a quick fix, since
            //this whole code is just a quick fix
            int ndx = lastGoodID - 1 + 256;
            receivingFrames[ndx-- % 256].good = true;
            receivingFrames[ndx-- % 256].good = true;
            receivingFrames[ndx   % 256].good = true;
        }
    }
}

//https://www.khanacademy.org/computing/computer-science/cryptography/modarithmetic/a/modular-addition-and-subtraction
static int8_t diffFrameIDs(uint8_t incoming, uint8_t old) {
    int diff = (int)incoming - (int)old;
    uint8_t mdiff = diff % 256;
    return (int8_t)mdiff;
}

void Network::RecieveUDP() {
    int ret;
    uint8_t buf[UDP_PACKET_SIZE];
    struct sockaddr_storage remote_addr;
    socklen_t remote_addr_len = sizeof(remote_addr);

    ret = recvfrom(udp_sock, buf, UDP_PACKET_SIZE, 0, (struct sockaddr*)&remote_addr, &remote_addr_len);
    if (ret <= 0) {
        if (quit) return;
        NetworkError("RecieveUDP failed");
        return;
    }

    uint8_t id = buf[0];
    uint8_t flags = buf[1];
    uint8_t fmt = buf[2];
    uint8_t seq = buf[3];

    int isTop = flags & 1;
    int lastPacket = flags & 0x10;

    uint8_t* jpeg_data = buf + 4;
    uint jpeg_size = ret - 4;
    uint jpeg_offset = seq * (UDP_PACKET_SIZE - 4);

    auto& lastGoodID = (isTop) ? lastGoodID_top : lastGoodID_btm;

    if (diffFrameIDs(id, lastGoodID) < 1) {
        if (diffFrameIDs(id, lastGoodID) == 0) {
            printf("[Network] BUG: received seq for previous frame\n");
        }
        printf("[Network] Discarding late seq %d for frame %d, last frame is %d\n", seq, id, lastGoodID);
        return;
    }

    auto& jpeg = getJpegForID(id, isTop);
    jpeg.seq[seq] = true;
    if (jpeg.data.size() < (jpeg_offset + jpeg_size)) {
        jpeg.data.resize(jpeg_offset + jpeg_size);
        //printf("resizing jpeg to %ld bytes for frame %d\n", jpeg.data.size(), id);
    }
    std::copy_n(jpeg_data, jpeg_size, std::next(jpeg.data.begin(), jpeg_offset));

    if (lastPacket) {
        //printf("pending frame %d on seq %d (last: %d)\n", id, seq, lastGoodID);
        jpeg.seq_last = seq;
        jpeg.pending = true;
    }

    checkGoodJpeg(jpeg, isTop);
}

void Network::SendRemotePlay(uint8_t priority, uint8_t priorityFactor, uint8_t jpegQuality, uint8_t QoS) {
    Packet pac = {
        .magic = NativeToLE(0x12345678),
        .seq = NativeToLE(1),
        .type = NativeToLE(0),
        .cmd = NativeToLE(901),
        .args = { .RemotePlay = {
            .mode = NativeToLE(priority << 8 | priorityFactor),
            .quality = NativeToLE(jpegQuality),
            .qos = NativeToLE(QoS*2 << 16),
        }},

        .length = NativeToLE(0),
    };
    send(ds_sock, &pac, sizeof(pac), 0);
}

int Network::SendInputRedirection(Input::InputState input) {
    int ret;
    if (input_sock < 0) {
        input_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (input_sock < 0) return errno;
    }
    uint32_t packet[] = {
        [0] = NativeToLE(input.buttons.data),
        [1] = NativeToLE(input.touch.data),
        [2] = NativeToLE(input.circle.data),
        [3] = NativeToLE(input.pro.data),
        [4] = NativeToLE(input.buttons_sys.data),
    };
    struct sockaddr_in ds_addr_input = ds_addr;
    ds_addr_input.sin_port = htons(4950);
    ret = sendto(input_sock, packet, sizeof(packet), 0, (struct sockaddr*)&ds_addr_input, sizeof(ds_addr_input));
    /*printf("input: %08X-%08X-%08X-%08X-%08X %d %d\n", packet[0], packet[1], packet[2], packet[3], packet[4],
        ret, errno
    );*/
    if (ret < 0) {
        return errno;
    } else return 0;
}

int Network::SendHeartbeat() {
    Packet pac = {
        .magic = NativeToLE(0x12345678),
        .seq = NativeToLE(1),
        .type = NativeToLE(0),
        .cmd = NativeToLE(0),
        .args = { .raw = { 0 } },

        .length = NativeToLE(0),
    };
    int ret = send(ds_sock, &pac, sizeof(pac), 0);
    if (ret < 0) {
        return errno;
    } else return 0;
}

static void heartbeatLoop() {
    while (!quit) {
        SendHeartbeat();
        sleep(1);
    }
}

static std::mutex lastInputMtx;
static Input::InputState lastInput;
static bool lastInputDirty = false;
static void inputLoop(int input_ratelimit_us, int input_pollrate_us) {
    while (!quit) {
        if (lastInputDirty) {
            {
                const std::lock_guard<std::mutex> lock(lastInputMtx);
                SendInputRedirection(lastInput);
                lastInputDirty = false;
            } //mutex release
            usleep(input_ratelimit_us);
        } else {
            usleep(input_pollrate_us);
        }
    }
}

void Network::mainLoop(const Config::NetworkConfig* config) {
    //init bits
    for (auto& jpeg : receivingFrames_top) {
        initJpeg(jpeg);
    }
    for (auto& jpeg : receivingFrames_btm) {
        initJpeg(jpeg);
    }

    //connect
    ListenUDP();
    while (udp_sock < 0 && !quit) {
        sleep(1);
        connect_attempts++;
        ListenUDP();
    }
    ConnectDS(config->host);
    while (ds_sock < 0 && !quit) {
        sleep(5);
        connect_attempts++;
        ConnectDS(config->host);
    }
    if (quit) {
        printf("[Network] quit requested\n");
        if (ds_sock >= 0) {
            printf("[Network] Tearing down ds sock\n");
            close(ds_sock);
        }
        if (udp_sock >= 0) {
            printf("[Network] Tearing down udp sock\n");
            close(udp_sock);
        }
        printf("[Network] bye!\n");
        return;
    }

    state = CONNECTED_WAIT;
    sleep(2); //I know, I know

    SendRemotePlay(config->priority, config->priorityFactor, config->jpegQuality, config->QoS);

    for (int i = 0; i < 2 && !quit; i++) {
        SendHeartbeat();
        sleep(1);
    }
    std::thread heartbeatThread(heartbeatLoop);
    std::thread inputThread(inputLoop, config->input_ratelimit_us, config->input_pollrate_us);
    while (!quit) {
        RecieveUDP();
    }
    inputThread.join();
    heartbeatThread.join();

    if (ds_sock >= 0) {
        close(ds_sock);
        ds_sock = -1;
    }
    if (udp_sock >= 0) {
        close(udp_sock);
        udp_sock = -1;
    }
}

void Network::Quit() {
    if (ds_sock >= 0) {
        close(ds_sock);
        ds_sock = -1;
    }
    if (udp_sock >= 0) {
        close(udp_sock);
        udp_sock = -1;
    }
    quit = true;
}

State Network::GetNetworkState() {
    return state;
}

int Network::GetConnectionAttempts() {
    return connect_attempts;
}

void Network::Input(Input::InputState input) {
    if (lastInput != input) {
        const std::lock_guard<std::mutex> lock(lastInputMtx);
        lastInput = input;
        lastInputDirty = true;
    }
}


//there's a lot of race conditions here.
uint8_t Network::GetTopJPEGID() {
    return lastGoodID_top;
}
std::vector<uint8_t>& Network::GetTopJPEG(uint8_t id) {
    return receivingFrames_top[id].data;
}
uint8_t Network::GetBtmJPEGID() {
    return lastGoodID_btm;
}
std::vector<uint8_t>& Network::GetBtmJPEG(uint8_t id) {
    return receivingFrames_btm[id].data;
}
