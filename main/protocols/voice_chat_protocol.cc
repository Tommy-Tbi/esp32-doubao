#include "voice_chat_protocol.h"
#include "board.h"
#include "system_info.h"
#include "settings.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>

#ifdef CONFIG_VOICE_CHAT_SERVER_URL
#define DEFAULT_SERVER_URL CONFIG_VOICE_CHAT_SERVER_URL
#else
#define DEFAULT_SERVER_URL "wss://your-server.com/ws"
#endif

#define TAG "VoiceChat"

VoiceChatProtocol::VoiceChatProtocol() {
    event_group_handle_ = xEventGroupCreate();
}

VoiceChatProtocol::~VoiceChatProtocol() {
    vEventGroupDelete(event_group_handle_);
}

bool VoiceChatProtocol::Start() {
    // Only connect to server when audio channel is needed
    return true;
}

bool VoiceChatProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    // 直接发送Opus音频数据
    return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
}

bool VoiceChatProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        return false;
    }

    return true;
}

bool VoiceChatProtocol::IsAudioChannelOpened() const {
    return audio_channel_opened_ && websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void VoiceChatProtocol::CloseAudioChannel() {
    audio_channel_opened_ = false;
    websocket_.reset();
}

bool VoiceChatProtocol::OpenAudioChannel() {
    Settings settings("voice_chat", false);
    std::string url = settings.GetString("url");
    if (url.empty()) {
        url = DEFAULT_SERVER_URL;
    }

    error_occurred_ = false;

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    // 设置设备信息头
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());

    // 处理接收到的消息
    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        HandleIncomingMessage(data, len, binary);
    });

    // 处理连接断开
    websocket_->OnDisconnected([this]() {
        HandleConnectionClosed();
    });

    ESP_LOGI(TAG, "Connecting to voice chat server: %s", url.c_str());
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to voice chat server, code=%d", websocket_->GetLastError());
        error_occurred_ = true;
        return false;
    }

    // 发送hello消息
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "device_id", SystemInfo::GetMacAddress().c_str());
    cJSON_AddNumberToObject(root, "sample_rate", 16000);

    char *json_str = cJSON_PrintUnformatted(root);
    bool send_ok = SendText(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);

    if (!send_ok) {
        return false;
    }

    audio_channel_opened_ = true;

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

void VoiceChatProtocol::HandleIncomingMessage(const char* data, size_t len, bool binary) {
    last_incoming_time_ = std::chrono::steady_clock::now();

    if (binary) {
        // 二进制数据：Opus音频，直接播放
        if (on_incoming_audio_ != nullptr) {
            on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                .sample_rate = server_sample_rate_,
                .frame_duration = server_frame_duration_,
                .timestamp = 0,
                .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)
            }));
        }
    } else {
        // JSON文本消息
        auto root = cJSON_Parse(data);
        if (root == nullptr) {
            ESP_LOGE(TAG, "Failed to parse JSON: %s", data);
            return;
        }

        auto type = cJSON_GetObjectItem(root, "type");
        if (cJSON_IsString(type)) {
            if (strcmp(type->valuestring, "hello") == 0) {
                // 服务器hello响应
                ESP_LOGI(TAG, "Received server hello");
            } else if (on_incoming_json_ != nullptr) {
                // 其他JSON消息传递给回调
                on_incoming_json_(root);
            }
        }

        cJSON_Delete(root);
    }
}

void VoiceChatProtocol::HandleConnectionClosed() {
    ESP_LOGI(TAG, "Voice chat connection closed");
    audio_channel_opened_ = false;
    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

void VoiceChatProtocol::HandleNetworkError(const std::string& message) {
    ESP_LOGE(TAG, "Network error: %s", message.c_str());
    error_occurred_ = true;
    if (on_network_error_ != nullptr) {
        on_network_error_(message);
    }
}