#ifndef VOICE_CHAT_PROTOCOL_H
#define VOICE_CHAT_PROTOCOL_H

#include <cJSON.h>
#include <string>
#include <functional>
#include <memory>
#include <chrono>

#include "protocol.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

class Network;
class WebSocket;

class VoiceChatProtocol : public Protocol {
public:
    VoiceChatProtocol();
    ~VoiceChatProtocol() override;

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;

protected:
    bool SendText(const std::string& text) override;

private:
    std::unique_ptr<WebSocket> websocket_;
    bool audio_channel_opened_ = false;
    EventGroupHandle_t event_group_handle_ = nullptr;

    void HandleIncomingMessage(const char* message, size_t len, bool is_binary);
    void HandleConnectionClosed();
    void HandleNetworkError(const std::string& message);
};

#endif  // VOICE_CHAT_PROTOCOL_H