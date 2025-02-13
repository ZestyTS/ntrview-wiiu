#pragma once

#include <string>
#include <coreinit/time.h>

#include "gfx/Gfx.hpp"
#include "gfx/font/Text.hpp"
#include "input/Input.hpp"
#include "Network.hpp"

class StatusOverlay {
public:
    StatusOverlay(const std::string& host);
    void Render();
    void ChangeHost(const std::string& host) {
        this->connecting_text.Change("Connecting to " + host);
    }
    void NetworkState(Network::State networkState) {
        this->network_state = networkState;
    };
    void InputPriorityMessage(Input::Priority priority, Input::ExtType extension);

private:
    Network::State network_state;
    OSTime input_message_timeout;

    Text::Text connecting_text;
    Text::Text attempt_text;
    Text::Text connected_text;
    Text::Text bad_ip_text;
    Text::Text input_priority_text;
};
