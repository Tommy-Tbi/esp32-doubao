#include "douyin_voice_protocol.h"
#include "board.h"
#include "system_info.h"
#include "settings.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_crt_bundle.h>
#include <esp_random.h>

#ifdef CONFIG_DOUYIN_VOICE

#ifdef CONFIG_DOUYIN_ACCESS_KEY
#define DEFAULT_ACCESS_KEY CONFIG_DOUYIN_ACCESS_KEY
#else
#define DEFAULT_ACCESS_KEY ""
#endif

#ifdef CONFIG_DOUYIN_APP_ID
#define DEFAULT_APP_ID CONFIG_DOUYIN_APP_ID
#else
#define DEFAULT_APP_ID ""
#endif

// 豆包实时语音API
#define DOUYIN_WSS_URL "wss://openspeech.bytedance.com/api/v3/realtime/dialogue"
#define DOUYIN_RESOURCE_ID "volc.speech.dialog"
#define DOUYIN_APP_KEY "PlgvMymc7f3tQnJ6"

#define TAG "DouyinVoice"
const char* DOUYIN_VOICE_TAG = "DouyinVoice";

// Message Types
#define MSG_TYPE_FULL_CLIENT_REQUEST  0x01
#define MSG_TYPE_FULL_SERVER_RESPONSE 0x09
#define MSG_TYPE_AUDIO_ONLY_REQUEST   0x02
#define MSG_TYPE_AUDIO_ONLY_RESPONSE  0x0B
#define MSG_TYPE_ERROR                0x0F

// Event IDs
#define EVENT_START_CONNECTION   1
#define EVENT_FINISH_CONNECTION  2
#define EVENT_START_SESSION      100
#define EVENT_FINISH_SESSION     102
#define EVENT_TASK_REQUEST       200

// Server Event IDs
#define EVENT_CONNECTION_STARTED  50
#define EVENT_CONNECTION_FAILED   51
#define EVENT_CONNECTION_FINISHED 52
#define EVENT_SESSION_STARTED     150
#define EVENT_SESSION_FINISHED    152
#define EVENT_SESSION_FAILED      153
#define EVENT_TTS_SENTENCE_START  350
#define EVENT_TTS_SENTENCE_END    351
#define EVENT_TTS_RESPONSE        352
#define EVENT_TTS_ENDED           359
#define EVENT_ASR_INFO            450
#define EVENT_ASR_RESPONSE        451
#define EVENT_ASR_ENDED           459
#define EVENT_CHAT_RESPONSE       550
#define EVENT_CHAT_ENDED          559

// 生成UUID
static std::string GenerateUuid() {
    char uuid[37];
    uint32_t random[4];
    for (int i = 0; i < 4; i++) {
        random[i] = esp_random();
    }
    snprintf(uuid, sizeof(uuid), "%08lx-%04lx-%04lx-%04lx-%08lx%04lx",
             (unsigned long)random[0],
             (unsigned long)((random[1] >> 16) & 0xFFFF),
             (unsigned long)(random[1] & 0xFFFF),
             (unsigned long)((random[2] >> 16) & 0xFFFF),
             (unsigned long)(random[2] & 0xFFFF),
             (unsigned long)(random[3] & 0xFFFF));
    return std::string(uuid);
}

DouyinVoiceProtocol::DouyinVoiceProtocol() {
    event_group_handle_ = xEventGroupCreate();
    session_id_ = "";
}

DouyinVoiceProtocol::~DouyinVoiceProtocol() {
    vEventGroupDelete(event_group_handle_);
}

void DouyinVoiceProtocol::SetAppId(const std::string& app_id) {
    app_id_ = app_id;
}

void DouyinVoiceProtocol::SetAccessKey(const std::string& access_key) {
    access_key_ = access_key;
}

bool DouyinVoiceProtocol::Start() {
    return true;
}

// 构建二进制帧
std::vector<uint8_t> DouyinVoiceProtocol::BuildFrame(uint8_t msg_type, uint8_t msg_flags,
                                                       uint32_t event_id, const std::string& session_id,
                                                       const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;

    // Header (4 bytes)
    uint8_t header0 = (0x01 << 4) | 0x01;  // Protocol v1, Header size 4
    uint8_t header1 = (msg_type << 4) | msg_flags;
    uint8_t serialization_method = (msg_type == MSG_TYPE_AUDIO_ONLY_REQUEST) ? 0x00 : 0x01;
    uint8_t header2 = (serialization_method << 4) | 0x00;  // serialization, no compression
    uint8_t header3 = 0x00;  // Reserved

    frame.push_back(header0);
    frame.push_back(header1);
    frame.push_back(header2);
    frame.push_back(header3);

    // Event ID (4 bytes, big endian) - only if msg_flags has event flag
    if (msg_flags & 0x04) {
        frame.push_back((event_id >> 24) & 0xFF);
        frame.push_back((event_id >> 16) & 0xFF);
        frame.push_back((event_id >> 8) & 0xFF);
        frame.push_back(event_id & 0xFF);
    }

    // Session ID (only for session events)
    if (!session_id.empty() && (msg_flags & 0x04)) {
        uint32_t session_id_size = session_id.size();
        frame.push_back((session_id_size >> 24) & 0xFF);
        frame.push_back((session_id_size >> 16) & 0xFF);
        frame.push_back((session_id_size >> 8) & 0xFF);
        frame.push_back(session_id_size & 0xFF);
        for (char c : session_id) {
            frame.push_back((uint8_t)c);
        }
    }

    // Payload size (4 bytes, big endian)
    uint32_t payload_size = payload.size();
    frame.push_back((payload_size >> 24) & 0xFF);
    frame.push_back((payload_size >> 16) & 0xFF);
    frame.push_back((payload_size >> 8) & 0xFF);
    frame.push_back(payload_size & 0xFF);

    // Payload
    frame.insert(frame.end(), payload.begin(), payload.end());

    return frame;
}

// OGG demuxer state
static bool ogg_seen_head = false;
static bool ogg_seen_tags = false;
static int ogg_sample_rate = 24000;
static std::string current_sentence_text;  // Accumulate text for current sentence

// Reset OGG demuxer state for new TTS response
static void ResetOggDemuxer() {
    ogg_seen_head = false;
    ogg_seen_tags = false;
    ogg_sample_rate = 24000;
}

// Remove XML-style tags from text (e.g., <music>, </music>, <text>, etc.)
static std::string RemoveXmlTags(const std::string& text) {
    std::string result;
    result.reserve(text.size());

    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '<') {
            // Skip until closing '>'
            while (i < text.size() && text[i] != '>') {
                i++;
            }
            if (i < text.size()) {
                i++;  // Skip the '>'
            }
        } else {
            result += text[i];
            i++;
        }
    }

    return result;
}

// 解析OGG页面并提取Opus帧
static void DemuxOggOpus(const uint8_t* data, size_t len,
                         std::function<void(std::unique_ptr<AudioStreamPacket>)> callback) {
    size_t offset = 0;

    while (true) {
        // Find OGG page signature "OggS"
        size_t pos = static_cast<size_t>(-1);
        for (size_t i = offset; i + 4 <= len; ++i) {
            if (data[i] == 'O' && data[i+1] == 'g' && data[i+2] == 'g' && data[i+3] == 'S') {
                pos = i;
                break;
            }
        }
        if (pos == static_cast<size_t>(-1)) break;
        offset = pos;
        if (offset + 27 > len) break;

        const uint8_t* page = data + offset;
        uint8_t page_segments = page[26];
        size_t seg_table_off = offset + 27;
        if (seg_table_off + page_segments > len) break;

        // Calculate body size from segment table
        size_t body_size = 0;
        for (size_t i = 0; i < page_segments; ++i) body_size += page[27 + i];

        size_t body_off = seg_table_off + page_segments;
        if (body_off + body_size > len) break;

        // Parse packets using lacing
        size_t cur = body_off;
        size_t seg_idx = 0;
        while (seg_idx < page_segments) {
            size_t pkt_len = 0;
            size_t pkt_start = cur;
            bool continued = false;
            do {
                uint8_t l = page[27 + seg_idx++];
                pkt_len += l;
                cur += l;
                continued = (l == 255);
            } while (continued && seg_idx < page_segments);

            if (pkt_len == 0) continue;
            const uint8_t* pkt_ptr = data + pkt_start;

            if (!ogg_seen_head) {
                // Parse OpusHead packet
                if (pkt_len >= 19 && std::memcmp(pkt_ptr, "OpusHead", 8) == 0) {
                    ogg_seen_head = true;
                    if (pkt_len >= 16) {
                        // Read input sample rate (little-endian)
                        ogg_sample_rate = pkt_ptr[12] | (pkt_ptr[13] << 8) |
                                        (pkt_ptr[14] << 16) | (pkt_ptr[15] << 24);
                        ESP_LOGI(TAG, "OpusHead: sample_rate=%d", ogg_sample_rate);
                    }
                }
                continue;
            }
            if (!ogg_seen_tags) {
                // Expect OpusTags in second packet
                if (pkt_len >= 8 && std::memcmp(pkt_ptr, "OpusTags", 8) == 0) {
                    ogg_seen_tags = true;
                }
                continue;
            }

            // Audio packet (Opus frame)
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = ogg_sample_rate;
            packet->frame_duration = 60;  // Default Opus frame duration
            packet->timestamp = 0;
            packet->payload.resize(pkt_len);
            std::memcpy(packet->payload.data(), pkt_ptr, pkt_len);
            callback(std::move(packet));
        }

        offset = body_off + body_size;
    }
}

// 解析服务器响应
void DouyinVoiceProtocol::ParseServerFrame(const uint8_t* data, size_t len) {
    uint8_t msg_type_peek = (len >= 2) ? ((data[1] >> 4) & 0x0F) : 0;
    if (len < 4) return;

    uint8_t header1 = data[1];
    uint8_t header2 = data[2];

    uint8_t msg_type = (header1 >> 4) & 0x0F;
    uint8_t msg_flags = header1 & 0x0F;
    uint8_t serialization = (header2 >> 4) & 0x0F;

    size_t offset = 4;

    // Parse event ID if present
    uint32_t event_id = 0;
    if (msg_flags & 0x04) {
        if (offset + 4 > len) return;
        event_id = (data[offset] << 24) | (data[offset+1] << 16) |
                   (data[offset+2] << 8) | data[offset+3];
        offset += 4;
    }

    // Parse session ID if present
    std::string recv_session_id;
    if ((msg_flags & 0x04) && (event_id >= 100)) {
        if (offset + 4 > len) return;
        uint32_t session_id_size = (data[offset] << 24) | (data[offset+1] << 16) |
                                   (data[offset+2] << 8) | data[offset+3];
        offset += 4;
        if (offset + session_id_size > len) return;
        recv_session_id = std::string((const char*)(data + offset), session_id_size);
        (void)recv_session_id;  // Session ID received from server
        offset += session_id_size;
    }

    // Parse payload
    if (offset + 4 > len) return;
    uint32_t payload_size = (data[offset] << 24) | (data[offset+1] << 16) |
                            (data[offset+2] << 8) | data[offset+3];
    offset += 4;

    if (offset + payload_size > len) return;
    const uint8_t* payload_data = data + offset;

    // Handle by message type
    if (msg_type == MSG_TYPE_AUDIO_ONLY_RESPONSE) {
        // Binary audio data
        if (on_incoming_audio_ != nullptr && payload_size > 0) {
            // Detect format: OGG files start with "OggS"
            bool is_ogg = (payload_size >= 4 && payload_data[0] == 'O' &&
                          payload_data[1] == 'g' && payload_data[2] == 'g' && payload_data[3] == 'S');

            // 调试：打印所有收到的音频帧
            ESP_LOGI(TAG, "Audio frame: size=%u, is_ogg=%d, first_bytes=0x%02X%02X%02X%02X",
                     (unsigned int)payload_size, is_ogg,
                     payload_data[0], payload_data[1], payload_data[2], payload_data[3]);

            if (is_ogg) {
                // OGG/Opus format - demux and extract Opus frames
                static int ogg_recv_count = 0;
                if (++ogg_recv_count == 1) {
                    ESP_LOGI(TAG, "Received OGG/Opus audio (first frame, %u bytes)", payload_size);
                }
                DemuxOggOpus(payload_data, payload_size, [this](std::unique_ptr<AudioStreamPacket> packet) {
                    on_incoming_audio_(std::move(packet));
                });
            } else {
                // PCM format (as configured in StartSession)
                static int pcm_recv_count = 0;
                if (++pcm_recv_count % 20 == 1) {
                    ESP_LOGI(TAG, "Received PCM audio #%d (%u bytes)", pcm_recv_count, payload_size);
                }
                auto packet = std::make_unique<AudioStreamPacket>();
                packet->sample_rate = 24000;
                packet->frame_duration = payload_size / 48;  // 24000Hz * 2 bytes = 48 bytes per ms
                packet->format = kAudioFormatPcm;
                packet->payload = std::vector<uint8_t>(payload_data, payload_data + payload_size);
                on_incoming_audio_(std::move(packet));
            }
        }
    } else if (msg_type == MSG_TYPE_FULL_SERVER_RESPONSE && serialization == 0x01) {
        // JSON response
        std::string json_str((const char*)payload_data, payload_size);
        // 截断长JSON避免刷屏（ChatResponse可能包含长文本）
        if (json_str.size() > 120) {
            ESP_LOGI(TAG, "Server event %u: %.120s...", event_id, json_str.c_str());
        } else {
            ESP_LOGI(TAG, "Server event %u: %s", event_id, json_str.c_str());
        }

        auto root = cJSON_Parse(json_str.c_str());
        if (root) {
            HandleServerEvent(event_id, root);
            cJSON_Delete(root);
        }
    } else if (msg_type == MSG_TYPE_ERROR) {
        std::string json_str((const char*)payload_data, payload_size);
        ESP_LOGE(TAG, "Error: %s", json_str.c_str());
        auto root = cJSON_Parse(json_str.c_str());
        if (root) {
            auto error = cJSON_GetObjectItem(root, "error");
            if (error && cJSON_IsString(error)) {
                HandleNetworkError(error->valuestring);
            }
            cJSON_Delete(root);
        }
    }
}

void DouyinVoiceProtocol::HandleServerEvent(uint32_t event_id, cJSON* root) {
    switch (event_id) {
        case EVENT_CONNECTION_STARTED:
            ESP_LOGI(TAG, "Connection started");
            break;

        case EVENT_CONNECTION_FAILED: {
            auto error = cJSON_GetObjectItem(root, "error");
            if (error && cJSON_IsString(error)) {
                ESP_LOGE(TAG, "Connection failed: %s", error->valuestring);
                HandleNetworkError(error->valuestring);
            }
            break;
        }

        case EVENT_SESSION_STARTED: {
            // Reset OGG demuxer for new session
            ResetOggDemuxer();

            auto dialog_id = cJSON_GetObjectItem(root, "dialog_id");
            if (dialog_id && cJSON_IsString(dialog_id)) {
                ESP_LOGI(TAG, "Session started, dialog_id: %s", dialog_id->valuestring);
                dialog_id_ = dialog_id->valuestring;
            }
            session_started_ = true;
            audio_channel_opened_ = true;

            // 诊断：发送SayHello测试服务端是否响应非音频事件
            {
                std::string hello_json = "{\"content\":\"你好\"}";
                std::vector<uint8_t> hello_payload(hello_json.begin(), hello_json.end());
                auto hello_frame = BuildFrame(MSG_TYPE_FULL_CLIENT_REQUEST, 0x04, 300, session_id_, hello_payload);
                ESP_LOGI(TAG, "Sending SayHello (event=300) to test server responsiveness...");
                if (websocket_) {
                    websocket_->Send(hello_frame.data(), hello_frame.size(), true);
                }
            }

            if (on_audio_channel_opened_ != nullptr) {
                on_audio_channel_opened_();
            }
            break;
        }

        case EVENT_SESSION_FAILED: {
            auto error = cJSON_GetObjectItem(root, "error");
            if (error && cJSON_IsString(error)) {
                ESP_LOGE(TAG, "Session failed: %s", error->valuestring);
                HandleNetworkError(error->valuestring);
            }
            break;
        }

        case EVENT_ASR_INFO: {
            auto question_id = cJSON_GetObjectItem(root, "question_id");
            if (question_id && cJSON_IsString(question_id)) {
                question_id_ = question_id->valuestring;
            }
            // 用户在TTS播放期间说话，通知应用层中断TTS
            ESP_LOGI(TAG, "ASR_INFO: user speech detected during TTS, aborting TTS");
            if (on_incoming_json_ != nullptr) {
                cJSON* wrapper = cJSON_CreateObject();
                cJSON_AddStringToObject(wrapper, "type", "tts");
                cJSON_AddStringToObject(wrapper, "state", "stop");
                cJSON_AddStringToObject(wrapper, "reason", "user_interrupt");  // 区分用户中断
                on_incoming_json_(wrapper);
                cJSON_Delete(wrapper);
            }
            break;
        }

        case EVENT_ASR_RESPONSE: {
            auto results = cJSON_GetObjectItem(root, "results");
            if (results && cJSON_IsArray(results)) {
                cJSON* item = cJSON_GetArrayItem(results, 0);
                if (item) {
                    auto text = cJSON_GetObjectItem(item, "text");
                    auto is_interim = cJSON_GetObjectItem(item, "is_interim");
                    if (text && cJSON_IsString(text)) {
                        bool is_interim_result = is_interim && is_interim->valueint;

                        // Only send final results (non-interim) to avoid duplicate UI messages
                        if (!is_interim_result) {
                            ESP_LOGI(TAG, "ASR final: %s", text->valuestring);
                            // Notify via JSON callback
                            if (on_incoming_json_ != nullptr) {
                                cJSON* wrapper = cJSON_CreateObject();
                                cJSON_AddStringToObject(wrapper, "type", "stt");
                                cJSON_AddStringToObject(wrapper, "text", text->valuestring);
                                on_incoming_json_(wrapper);
                                cJSON_Delete(wrapper);
                            }
                        }
                    }
                }
            }
            break;
        }

        case EVENT_ASR_ENDED:
            break;

        case EVENT_CHAT_RESPONSE: {
            auto content = cJSON_GetObjectItem(root, "content");
            if (content && cJSON_IsString(content)) {
                // Remove XML tags like <music>, </music>, etc.
                std::string filtered_text = RemoveXmlTags(content->valuestring);
                // Accumulate for sentence_end
                current_sentence_text += filtered_text;

                // Send text to application for display
                if (on_incoming_json_ != nullptr) {
                    cJSON* wrapper = cJSON_CreateObject();
                    cJSON_AddStringToObject(wrapper, "type", "chat_text");
                    cJSON_AddStringToObject(wrapper, "text", current_sentence_text.c_str());
                    on_incoming_json_(wrapper);
                    cJSON_Delete(wrapper);
                }
            }
            break;
        }

        case EVENT_TTS_SENTENCE_START: {
            ESP_LOGI(TAG, "TTS_SENTENCE_START event received");
            // Reset OGG demuxer state for new TTS audio stream
            ResetOggDemuxer();
            current_sentence_text.clear();  // Clear for new sentence

            // Extract text from TTS_SENTENCE_START event if available
            auto text_field = cJSON_GetObjectItem(root, "text");
            if (text_field && cJSON_IsString(text_field) && strlen(text_field->valuestring) > 0) {
                std::string text = RemoveXmlTags(text_field->valuestring);
                current_sentence_text = text;
                ESP_LOGI(TAG, "TTS text from start event: %s", text.c_str());
            }

            // Notify state change and clear UI text for streaming
            if (on_incoming_json_ != nullptr) {
                cJSON* wrapper = cJSON_CreateObject();
                cJSON_AddStringToObject(wrapper, "type", "tts");
                cJSON_AddStringToObject(wrapper, "state", "start");
                // Pass the text to application for display
                if (!current_sentence_text.empty()) {
                    cJSON_AddStringToObject(wrapper, "text", current_sentence_text.c_str());
                }
                on_incoming_json_(wrapper);
                cJSON_Delete(wrapper);
            }
            break;
        }

        case EVENT_TTS_SENTENCE_END: {
            ESP_LOGI(TAG, "TTS_SENTENCE_END event received, current_sentence_text: %s", current_sentence_text.c_str());
            // Log the complete sentence (text was already streamed via CHAT_RESPONSE)
            if (!current_sentence_text.empty()) {
                ESP_LOGI(TAG, "Reply: %s", current_sentence_text.c_str());
                current_sentence_text.clear();
            }
            // Notify application that sentence audio data is finished
            if (on_incoming_json_ != nullptr) {
                cJSON* wrapper = cJSON_CreateObject();
                cJSON_AddStringToObject(wrapper, "type", "tts");
                cJSON_AddStringToObject(wrapper, "state", "stop");
                on_incoming_json_(wrapper);
                cJSON_Delete(wrapper);
            }
            break;
        }

        case EVENT_TTS_ENDED: {
            ESP_LOGI(TAG, "TTS_ENDED event received, text: %s", current_sentence_text.c_str());
            // Note: This event is sent for EACH sentence in multi-sentence TTS
            // 重置问题ID
            question_id_.clear();

            // 注意：不要在这里发送任何消息给服务器，会导致死循环

            if (on_incoming_json_ != nullptr) {
                cJSON* wrapper = cJSON_CreateObject();
                cJSON_AddStringToObject(wrapper, "type", "tts");
                cJSON_AddStringToObject(wrapper, "state", "stop");
                on_incoming_json_(wrapper);
                cJSON_Delete(wrapper);
            }
            break;
        }

        case EVENT_CHAT_ENDED: {
            ESP_LOGI(TAG, "CHAT_ENDED event received");
            break;
        }

        case 154: { // Usage stats (unhandled but not critical)
            break;
        }

        case 599: { // DialogCommonError
            auto status_code = cJSON_GetObjectItem(root, "status_code");
            auto message = cJSON_GetObjectItem(root, "message");
            ESP_LOGE(TAG, "DialogCommonError: status_code=%s, message=%s",
                     status_code && cJSON_IsString(status_code) ? status_code->valuestring : "?",
                     message && cJSON_IsString(message) ? message->valuestring : "?");
            break;
        }

        default:
            ESP_LOGI(TAG, "Unhandled event: %u", event_id);
            break;
    }
}

bool DouyinVoiceProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }
    if (!session_started_ || !audio_channel_opened_ || error_occurred_) {
        return false;
    }

    // 检查音频数据
    if (packet->payload.empty()) {
        return true;  // 忽略空数据
    }

    // 打印要发送的音频数据信息
    ESP_LOGI(TAG, "[SendAudio] Sending audio to server, payload_size=%d", packet->payload.size());

    // 发送音频数据 (event=200, TaskRequest)
    std::vector<uint8_t> frame = BuildFrame(MSG_TYPE_AUDIO_ONLY_REQUEST, 0x04,
                                             EVENT_TASK_REQUEST, session_id_,
                                             packet->payload);

    return websocket_->Send(frame.data(), frame.size(), true);
}

bool DouyinVoiceProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    return websocket_->Send(text);
}

bool DouyinVoiceProtocol::IsAudioChannelOpened() const {
    return audio_channel_opened_ && websocket_ != nullptr && websocket_->IsConnected() &&
           !error_occurred_ && session_started_;
}

void DouyinVoiceProtocol::SignalNewQuestion() {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        ESP_LOGW(TAG, "SignalNewQuestion: websocket not connected");
        return;
    }
    if (!session_started_) {
        ESP_LOGW(TAG, "SignalNewQuestion: session not started");
        return;
    }

    ESP_LOGI(TAG, "Signaling new question to server");

    // 发送一个特殊事件通知服务器准备接收新的语音输入
    // 使用空JSON作为负载，event 300 是 SayHello/测试事件
    std::string hello_json = "{\"content\":\"\"}";
    std::vector<uint8_t> payload(hello_json.begin(), hello_json.end());

    auto frame = BuildFrame(MSG_TYPE_FULL_CLIENT_REQUEST, 0x04, 300, session_id_, payload);
    if (websocket_->Send(frame.data(), frame.size(), true)) {
        ESP_LOGI(TAG, "New question signal sent successfully");
    } else {
        ESP_LOGW(TAG, "Failed to send new question signal");
    }
}

bool DouyinVoiceProtocol::ReopenAudioChannel() {
    ESP_LOGI(TAG, "Reopening audio channel...");

    // 关闭现有通道
    CloseAudioChannel();

    // 等待一下让连接关闭
    vTaskDelay(pdMS_TO_TICKS(100));

    // 重新打开通道
    return OpenAudioChannel();
}

void DouyinVoiceProtocol::CloseAudioChannel() {
    // 发送 FinishSession
    if (websocket_ && websocket_->IsConnected() && session_started_) {
        std::string empty_json = "{}";
        std::vector<uint8_t> payload(empty_json.begin(), empty_json.end());
        auto frame = BuildFrame(MSG_TYPE_FULL_CLIENT_REQUEST, 0x04,
                                 EVENT_FINISH_SESSION, session_id_, payload);
        websocket_->Send(frame.data(), frame.size(), true);
    }

    // 关闭 WebSocket 连接
    if (websocket_) {
        websocket_->Close();
    }

    audio_channel_opened_ = false;
    session_started_ = false;
    session_id_ = "";
}

bool DouyinVoiceProtocol::OpenAudioChannel() {
    // 从NVS或配置获取认证信息
    Settings settings("douyin", false);
    std::string app_id = settings.GetString("app_id");
    std::string access_key = settings.GetString("access_key");

    if (app_id.empty()) {
        app_id = DEFAULT_APP_ID;
    }
    if (access_key.empty()) {
        access_key = DEFAULT_ACCESS_KEY;
    }

    if (app_id.empty() || access_key.empty()) {
        ESP_LOGE(TAG, "AppID or AccessKey is empty, please configure in menuconfig");
        return false;
    }

    // Reset error flag so a previous error doesn't permanently block the channel
    error_occurred_ = false;

    app_id_ = app_id;
    access_key_ = access_key;

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    // 设置认证Headers
    websocket_->SetHeader("X-Api-App-ID", app_id_.c_str());
    websocket_->SetHeader("X-Api-Access-Key", access_key_.c_str());
    websocket_->SetHeader("X-Api-Resource-Id", DOUYIN_RESOURCE_ID);
    websocket_->SetHeader("X-Api-App-Key", DOUYIN_APP_KEY);

    // 处理接收到的消息
    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (!binary) {
            // 文本消息，可能是错误或心跳响应
        } else {
            ParseServerFrame((const uint8_t*)data, len);
        }
    });

    // 处理连接断开
    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "WebSocket disconnected");
        audio_channel_opened_ = false;
        session_started_ = false;
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    ESP_LOGI(TAG, "Connecting to Douyin Realtime API: %s", DOUYIN_WSS_URL);
    if (!websocket_->Connect(DOUYIN_WSS_URL)) {
        ESP_LOGE(TAG, "Failed to connect to Douyin API, code=%d", websocket_->GetLastError());
        error_occurred_ = true;
        return false;
    }

    // 生成唯一的 session_id (UUID格式)
    session_id_ = GenerateUuid();

    // 1. 发送 StartConnection (event=1)
    {
        std::string empty_json = "{}";
        std::vector<uint8_t> payload(empty_json.begin(), empty_json.end());
        auto frame = BuildFrame(MSG_TYPE_FULL_CLIENT_REQUEST, 0x04,
                                 EVENT_START_CONNECTION, "", payload);
        if (!websocket_->Send(frame.data(), frame.size(), true)) {
            ESP_LOGE(TAG, "Failed to send StartConnection");
            return false;
        }
        ESP_LOGI(TAG, "Sent StartConnection");
    }

    // 2. 发送 StartSession (event=100)
    {
        cJSON *root = cJSON_CreateObject();

        // 注意：不设置 tts audio_config，服务端默认返回 OGG 封装的 Opus 音频
        // AudioService 的 Opus 解码器可以处理此格式

        // ASR配置 - 客户端发送Opus格式音频，需要配置audio_info
        cJSON *asr = cJSON_CreateObject();
        cJSON *audio_info = cJSON_CreateObject();
        cJSON_AddStringToObject(audio_info, "format", "speech_opus");  // 客户端发送Opus格式
        cJSON_AddNumberToObject(audio_info, "sample_rate", 16000);
        cJSON_AddNumberToObject(audio_info, "channel", 1);
        cJSON_AddItemToObject(asr, "audio_info", audio_info);
        cJSON *asr_extra = cJSON_CreateObject();
        cJSON_AddNumberToObject(asr_extra, "end_smooth_window_ms", 1500);
        cJSON_AddItemToObject(asr, "extra", asr_extra);
        cJSON_AddItemToObject(root, "asr", asr);

        // 注意：不设置 tts audio_config，服务端默认返回 OGG 封装的 Opus 音频
        // AudioService 的 Opus 解码器可以处理此格式

        // Dialog配置
        cJSON *dialog = cJSON_CreateObject();
        cJSON_AddStringToObject(dialog, "bot_name", "豆包");
        cJSON_AddStringToObject(dialog, "dialog_id", "");

        cJSON *extra = cJSON_CreateObject();
        cJSON_AddStringToObject(extra, "model", "1.2.1.1");  // O2.0版本
        cJSON_AddStringToObject(extra, "input_mod", "keep_alive");  // 防止TTS静音期间报错52000042
        cJSON_AddItemToObject(dialog, "extra", extra);

        cJSON_AddItemToObject(root, "dialog", dialog);

        char *json_str = cJSON_PrintUnformatted(root);
        std::vector<uint8_t> payload(json_str, json_str + strlen(json_str));
        cJSON_free(json_str);
        cJSON_Delete(root);

        auto frame = BuildFrame(MSG_TYPE_FULL_CLIENT_REQUEST, 0x04,
                                 EVENT_START_SESSION, session_id_, payload);
        if (!websocket_->Send(frame.data(), frame.size(), true)) {
            ESP_LOGE(TAG, "Failed to send StartSession");
            return false;
        }
        ESP_LOGI(TAG, "Sent StartSession with session_id: %s", session_id_.c_str());
    }

    // 等待会话建立
    int retry = 0;
    while (!session_started_ && retry < 50) {
        vTaskDelay(pdMS_TO_TICKS(100));
        retry++;
    }

    if (!session_started_) {
        ESP_LOGE(TAG, "Session start timeout, no SESSION_STARTED event from server");
        return false;
    }

    ESP_LOGI(TAG, "OpenAudioChannel success: session_id=%s, session_started=%d",
             session_id_.c_str(), session_started_ ? 1 : 0);

    return true;
}

void DouyinVoiceProtocol::HandleNetworkError(const std::string& message) {
    ESP_LOGE(TAG, "Network error: %s", message.c_str());
    error_occurred_ = true;
    if (on_network_error_ != nullptr) {
        on_network_error_(message);
    }
}
#endif