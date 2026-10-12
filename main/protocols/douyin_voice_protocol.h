#ifndef DOUYIN_VOICE_PROTOCOL_H
#define DOUYIN_VOICE_PROTOCOL_H

#include <cJSON.h>
#include <string>
#include <functional>
#include <memory>
#include <chrono>
#include <vector>

#include "protocol.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_log.h>

class Network;
class WebSocket;

// TAG for logging in header inline function
extern const char* DOUYIN_VOICE_TAG;

class DouyinVoiceProtocol : public Protocol {
public:
    DouyinVoiceProtocol();
    ~DouyinVoiceProtocol() override;

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;

    // 豆包协议不需要单独的 start/stop listening 命令，由服务端 VAD 控制
    void SendStartListening(ListeningMode mode) override {
        ESP_LOGI(DOUYIN_VOICE_TAG, "Douyin protocol: session already started, no need for start listening command");
    }
    void SendStopListening() override {
        ESP_LOGI(DOUYIN_VOICE_TAG, "Douyin protocol: stop listening is no-op (server VAD handles end of speech)");
    }

    // 设置认证信息
    void SetAppId(const std::string& app_id);
    void SetAccessKey(const std::string& access_key);

    // 发送新对话信号，告诉服务器准备接收新的语音输入
    void SignalNewQuestion();

    // 重新建立音频通道（在对话轮次之间使用）
    bool ReopenAudioChannel();

protected:
    bool SendText(const std::string& text) override;

private:
    std::unique_ptr<WebSocket> websocket_;
    bool audio_channel_opened_ = false;
    EventGroupHandle_t event_group_handle_ = nullptr;

    std::string app_id_;
    std::string access_key_;
    std::string session_id_;
    std::string dialog_id_;
    std::string question_id_;
    bool session_started_ = false;

    // 构建二进制帧
    std::vector<uint8_t> BuildFrame(uint8_t msg_type, uint8_t msg_flags,
                                     uint32_t event_id, const std::string& session_id,
                                     const std::vector<uint8_t>& payload);

    // 解析服务器帧
    void ParseServerFrame(const uint8_t* data, size_t len);

    // 处理服务器事件
    void HandleServerEvent(uint32_t event_id, cJSON* root);

    void HandleNetworkError(const std::string& message);
};

#endif  // DOUYIN_VOICE_PROTOCOL_H