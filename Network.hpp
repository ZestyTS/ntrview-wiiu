#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>
#include <array>
#include <mutex>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cerrno>

#include "input/Input.hpp"
#include "config/Config.hpp"

namespace Network {

typedef enum State {
    CONNECTING,
    CONNECTED_WAIT,
    CONNECTED_STREAMING,
    ERR_BAD_IP
} State;

void ConnectDS(const std::string host);
void ListenUDP();
void RecieveUDP();
void SendRemotePlay(uint8_t priority, uint8_t priorityFactor, uint8_t jpegQuality, uint8_t QoS);
int SendHeartbeat();
int SendInputRedirection(Input::InputState input);

State GetNetworkState();
int GetConnectionAttempts();

std::vector<uint8_t>& GetBtmJPEG(uint8_t id);
uint8_t GetBtmJPEGID();
std::vector<uint8_t>& GetTopJPEG(uint8_t id);
uint8_t GetTopJPEGID();

void mainLoop(const Config::NetworkConfig* config);
void Quit();

#define NetworkError(fmt) printf("[Network] " fmt ": %d\n", errno)
#define NetworkErrorF(fmt, ...) printf("[Network] " fmt ": %d\n", __VA_ARGS__, errno)

struct Packet {
    uint32_t magic;
    uint32_t seq;
    uint32_t type;
    uint32_t cmd;
    union {
        struct RemotePlay {
            uint32_t mode;
            uint32_t quality;
            uint32_t qos;
        } RemotePlay;
        uint32_t raw[16];
    } args;

    uint32_t length;
};

#define UDP_PACKET_SIZE (1448)

void Input(Input::InputState input);

} //namespace Network
