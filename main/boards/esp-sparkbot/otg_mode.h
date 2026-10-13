#ifndef SPARKBOT_OTG_MODE_H_
#define SPARKBOT_OTG_MODE_H_

#include <atomic>
#include <cstdint>
#include <functional>

#include "display.h"

// 退出 OTG 时调：持久化 NVS 标志 otg/disabled=1 后 esp_restart()
// 下次启动时 audio_service 会跳过 USB 音频初始化，设备以 JTAG 模式被 PC 识别
void OtgDisableAndRestart();

/*
 * OTG（USB 复合音频）模式管理：
 *
 *   - USB 主机（PC）接入  -> 自动进入 OTG 模式：
 *       设备作为 USB Speaker + USB Microphone 工作（见 main/usb_audio.cc），
 *       屏幕显示 OTG 提示界面。
 *   - 同时按下任意两个按键 -> 退出 OTG 模式：
 *       停止 USB 音频通路（麦克风不再上传、PC 音频不再从本地喇叭播出），
 *       设备回到正常的语音助手模式，屏幕弹出“已退出 OTG 模式”。
 *   - 退出后需要重新插拔 USB 才再次进入（避免误触反复进出）。
 */
class OtgMode {
public:
    static OtgMode& GetInstance();

    // 注册 USB 连接回调（追加监听者），并保存用于显示的屏幕对象
    void Begin(Display* display);

    bool IsUsbConnected() const { return usb_connected_.load(); }
    bool IsActive() const { return active_.load(); }
    bool IsBlocked() const { return blocked_.load(); }

    // 组合键触发退出，key_mask 为按下的按键位图（0 表示仅作为提示）
    void ExitByKeys(uint32_t key_mask);

    // 供上层查询的屏幕状态文本
    static const char* StatusText() { return "OTG 模式"; }

private:
    OtgMode() = default;

    void OnUsbConnection(bool connected);
    void Enter();
    void Leave(bool by_keys, uint32_t key_mask);

    void ShowOverlay();
    void HideOverlay();

    Display* display_ = nullptr;
    void* overlay_ = nullptr;  // lv_obj_t*，屏幕上的 OTG 提示界面
    std::atomic<bool> usb_connected_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> blocked_{false};  // 用户已按键退出，等待重新插拔
};

#endif  // SPARKBOT_OTG_MODE_H_