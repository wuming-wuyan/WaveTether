// 音频层：WinRT A2DP 接收 + WASAPI 会话音量 + PolicyConfig 切默认输出
#pragma once
#include <string>
#include <vector>
#include <windows.h>
#include <vector>
#include <windows.h>
#include <functional>

enum class LinkState { Idle, Connecting, Listening, Connected, Failed };

struct SinkDevice
{
    std::wstring id, name;
    // 蓝牙 MAC（12 位十六进制，如 "001122334455"）。
    // 名字会变，MAC 不会 —— 用它当设备的真实身份，名字只作显示。
    std::string mac;
};

struct PhoneSessionInfo
{
    bool found = false;
    bool playing = false;
    float volume = -1.f;
    bool muted = false;
    std::wstring instanceId;
    DWORD pid = 0;                 // 会话所属进程
    std::wstring displayName;      // 会话显示名（蓝牙 A2DP 会话会含设备名）
    bool isBt = false;             // 是否为蓝牙 A2DP 接收会话
    float peak = -1.f;             // 峰值表：>0 表示确实在出声（state 不可靠）
};

// ---- 蓝牙 A2DP 接收（WinRT） ----
class BtLink
{
public:
    bool Init();                                     // 加载 WinRT 工厂
    std::vector<SinkDevice> Enumerate();             // 枚举 A2DP Sink 设备
    void Connect(const std::wstring& deviceId, const std::wstring& deviceName = L"");  // 主动连接（异步，内部轮询）
    void Disconnect();                               // 断开
    void Tick();                                     // 主循环里轮询异步结果/状态
    LinkState State() const { return state_; }
    const wchar_t* StateText() const;
    const std::wstring& DeviceName() const { return deviceName_; }
    bool UserDisconnected() const { return userDisconnected_; }
    int Attempt() const { return attempt_; }
private:
    struct Impl; Impl* impl_ = nullptr;
    LinkState state_ = LinkState::Idle;
    std::wstring deviceName_;
    bool userDisconnected_ = false;
    int attempt_ = 0;
};

// ---- 系统输出设备（CoreAudio） ----
struct OutDevice { std::wstring id, name; bool active; };

class OutputAudio
{
public:
    bool Init();
    std::vector<OutDevice> RenderDevices();          // 所有渲染端点（含状态）
    std::wstring DefaultRenderId();
    bool SetDefaultRender(const std::wstring& id);   // IPolicyConfig
    // 手机音频会话（挂在默认输出上）
    // 默认输出端点的音量（= Windows 系统音量）

    // 多设备：返回所有匹配的会话（每台设备一个 scrcpy → 一个会话）
    // 枚举当前默认输出设备上的全部音频会话（用于逐设备音量）
    std::vector<PhoneSessionInfo> AllSessions();
    bool SetSessionVolume(const std::wstring& instanceId, float vol);
    bool SetSessionMute(const std::wstring& instanceId, bool mute);
};
