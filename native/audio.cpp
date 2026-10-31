// 音频层：WinRT A2DP 接收 + WASAPI 会话音量 + PolicyConfig 切默认输出
#include "audio.h"
#include "perf.h"
#include <thread>

#include <winrt/base.h>
#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <roapi.h>

#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <endpointvolume.h>

#include <string>
#include <vector>

using namespace winrt;
namespace wa  = winrt::Windows::Media::Audio;
namespace wde = winrt::Windows::Devices::Enumeration;
namespace wf  = winrt::Windows::Foundation;

// ============================================================ BtLink

struct BtLink::Impl
{
    wa::AudioPlaybackConnection conn{ nullptr };
    std::wstring deviceId;
    wf::IAsyncOperation<wa::AudioPlaybackConnectionOpenResult> openOp{ nullptr };
    wf::IAsyncAction startOp{ nullptr };
    bool started = false;       // Start 只做一次
    int startWait = 0;
    int stateTick = 0;      // conn.State() 限流（每 4 tick 查一次）
    int openWait = 0;
    int ticksToNextOpen = 0;    // Open 退避计时（250ms/tick）
    int openAttempts = 0;
};

static const wchar_t* StateName(LinkState s)
{
    switch (s)
    {
    case LinkState::Connecting: return L"连接中…";
    case LinkState::Listening:  return L"等待手机连接…";
    case LinkState::Connected:  return L"已连接";
    case LinkState::Failed:     return L"连接失败";
    default:                    return L"未连接";
    }
}

const wchar_t* BtLink::StateText() const { return StateName(state_); }

bool BtLink::Init()
{
    try { ::RoInitialize(RO_INIT_MULTITHREADED); } catch (...) {}
    if (!impl_) impl_ = new Impl();
    return true;
}

// 从 PnP 设备 Id 里抠出蓝牙 MAC。
// Id 形如：
//   \?\BTHENUM#{0000110a-...}_VID&000100e0_PID&0000#8&129df44a&0&001122334455_C00000000#{...}\SNK
//                                              ^^^^^^^^^^^^ MAC
// 取「& 之后正好 12 个十六进制字符、紧跟 _」的那一段。
static std::string MacFromDeviceId(const std::wstring& id)
{
    auto isHex = [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
    };
    for (size_t i = 0; i + 14 <= id.size(); ++i)
    {
        if (id[i] != L'&') continue;
        bool ok = true;
        for (size_t k = 1; k <= 12; ++k) if (!isHex(id[i + k])) { ok = false; break; }
        if (!ok || id[i + 13] != L'_') continue;
        std::string mac;
        for (size_t k = 1; k <= 12; ++k) mac += (char)towupper(id[i + k]);
        return mac;
    }
    return std::string();
}

std::vector<SinkDevice> BtLink::Enumerate()
{
    std::vector<SinkDevice> out;
    try
    {
        auto sel = wa::AudioPlaybackConnection::GetDeviceSelector();
        wde::DeviceInformationCollection infos = wde::DeviceInformation::FindAllAsync(sel).get();
        for (auto const& d : infos)
        {
            SinkDevice s;
            s.id = std::wstring(d.Id().c_str());
            s.mac = MacFromDeviceId(s.id);
            s.name = std::wstring(d.Name().c_str());
            if (s.name.empty()) s.name = L"(未命名设备)";
            out.push_back(std::move(s));
        }
    }
    catch (hresult_error const& e)
    {
        // 枚举失败时界面会显示「还没有配对的设备」，等于把故障伪装成正常状态。
        // 至少留一行日志，方便排查。
        OutputDebugStringW((std::wstring(L"Enumerate: ") + e.message().c_str()).c_str());
        PerfLog("BT 枚举失败：%s", PerfU8(e.message().c_str()).c_str());
    }
    return out;
}

void BtLink::Connect(const std::wstring& deviceId, const std::wstring& deviceName)
{
    if (!impl_) Init();
    userDisconnected_ = false;
    attempt_ = 0;
    deviceName_ = deviceName;   // 记住蓝牙名：用于精确匹配对应 ADB 设备

    // 重建连接对象（换设备时必须重建）
    if (impl_->conn)
    {
        try { impl_->conn.Close(); } catch (...) {}
        impl_->conn = nullptr;
    }
    impl_->openOp = nullptr;
    impl_->startOp = nullptr;
    impl_->started = false;
    impl_->startWait = 0;
    impl_->openWait = 0;
    impl_->ticksToNextOpen = 0;
    impl_->openAttempts = 0;
    impl_->deviceId = deviceId;

    try
    {
        impl_->conn = wa::AudioPlaybackConnection::TryCreateFromId(deviceId);
    }
    catch (...) { impl_->conn = nullptr; }

    if (!impl_->conn)
    {
        // 蓝牙链路这一路以前几乎不打日志，出问题只能靠猜。
        PerfLog("BT 连接失败：AudioPlaybackConnection 创建不出来（设备=%s）", PerfU8(deviceName_).c_str());
        state_ = LinkState::Failed;
        return;
    }
    PerfLog("BT 开始连接：%s", PerfU8(deviceName_).c_str());
    state_ = LinkState::Connecting;
}

void BtLink::Disconnect()
{
    if (!impl_) return;
    userDisconnected_ = true;
    attempt_ = 0;
    impl_->openOp = nullptr;
    impl_->startOp = nullptr;
    if (impl_->conn)
    {
        try { impl_->conn.Close(); } catch (...) {}
        impl_->conn = nullptr;
    }
    PerfLog("BT 断开（用户主动）：%s", PerfU8(deviceName_).c_str());
    state_ = LinkState::Idle;
}

void BtLink::Tick()
{
    if (!impl_ || !impl_->conn) return;
    auto& im = *impl_;

    // 已连上：什么都不做，避免打断音频流
    if (im.conn.State() == wa::AudioPlaybackConnectionState::Opened)
    {
        attempt_ = 0;
        im.openAttempts = 0;
        state_ = LinkState::Connected;
        return;
    }
    if (state_ == LinkState::Idle) return;

    // ---- 1) Start：只做一次（重复调用会打断连接建立） ----
    if (!im.started)
    {
        if (!im.startOp)
        {
            try { im.startOp = im.conn.StartAsync(); im.startWait = 0; }
            catch (...) { im.started = true; }
        }
        if (im.startOp)
        {
            auto st = im.startOp.Status();
            if (st == wf::AsyncStatus::Completed)
            {
                try { im.startOp.GetResults(); } catch (...) {}
                im.startOp = nullptr;
                im.started = true;
            }
            else if (st == wf::AsyncStatus::Error || st == wf::AsyncStatus::Canceled)
            {
                im.startOp = nullptr;
                im.started = true;
            }
            else if (++im.startWait > 20) { im.startOp = nullptr; im.started = true; }   // 20 秒超时
        }
    }

    // ---- 2) Open：带退避重试，不并发 ----
    if (im.ticksToNextOpen > 0) { im.ticksToNextOpen--; }
    else if (!im.openOp)
    {
        try { im.openOp = im.conn.OpenAsync(); im.openWait = 0; } catch (...) {}
    }

    if (im.openOp)
    {
        auto st = im.openOp.Status();
        if (st == wf::AsyncStatus::Completed)
        {
            try { im.openOp.GetResults(); } catch (...) {}
            im.openOp = nullptr;
            if (im.conn.State() == wa::AudioPlaybackConnectionState::Opened)
            {
                attempt_ = 0;
                im.openAttempts = 0;
                state_ = LinkState::Connected;
            }
            else
            {
                // 手机没接受主动连接 → 转监听，按指数退避再试
                state_ = LinkState::Listening;
                im.openAttempts++;
                attempt_ = im.openAttempts;
                int shift = im.openAttempts - 1;
                if (shift > 3) shift = 3;
                int seconds = 1 << shift;          // 1,2,4,8
                im.ticksToNextOpen = seconds;      // 单位：秒（Tick 每秒跑一次状态机）
            }
        }
        else if (st == wf::AsyncStatus::Error || st == wf::AsyncStatus::Canceled)
        {
            im.openOp = nullptr;
            state_ = LinkState::Listening;
            im.openAttempts++;
            attempt_ = im.openAttempts;
            int shift = im.openAttempts - 1;
            if (shift > 3) shift = 3;
            im.ticksToNextOpen = (1 << shift);
        }
        else if (++im.openWait > 30)   // 30 秒超时
        {
            im.openOp = nullptr;
            im.ticksToNextOpen = 8;   // 8 秒后再试
        }
    }
}

// ============================================================ IPolicyConfig

static const GUID kCLSID_PolicyConfigClient =
    { 0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };
static const GUID kIID_IPolicyConfig =
    { 0xf8679f50, 0x850a, 0x41cf, { 0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8 } };

MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfigX : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(LPCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(LPCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(LPCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(LPCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(LPCWSTR, INT, INT64*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(LPCWSTR, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(LPCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(LPCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(LPCWSTR, BOOL, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(LPCWSTR, BOOL, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(LPCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(LPCWSTR, INT) = 0;
};

static IPolicyConfigX* g_policy = nullptr;

// ============================================================ OutputAudio

bool OutputAudio::Init() { return true; }

static IMMDeviceEnumerator* NewEnumerator()
{
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)))
        return nullptr;
    return en;
}

std::vector<OutDevice> OutputAudio::RenderDevices()
{
    std::vector<OutDevice> out;
    IMMDeviceEnumerator* en = NewEnumerator();
    if (!en) return out;

    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, &col)))
    {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; i++)
        {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d))) continue;

            LPWSTR id = nullptr;
            DWORD state = 0;
            if (SUCCEEDED(d->GetId(&id)) && SUCCEEDED(d->GetState(&state)))
            {
                std::wstring name;
                IPropertyStore* ps = nullptr;
                if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)))
                {
                    PROPVARIANT v;
                    PropVariantInit(&v);
                    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
                        name = v.pwszVal;
                    PropVariantClear(&v);
                    ps->Release();
                }
                OutDevice od;
                od.id = id;
                od.name = name;
                od.active = (state == DEVICE_STATE_ACTIVE);
                out.push_back(std::move(od));
                CoTaskMemFree(id);
            }
            d->Release();
        }
        col->Release();
    }
    en->Release();
    return out;
}

std::wstring OutputAudio::DefaultRenderId()
{
    std::wstring id;
    IMMDeviceEnumerator* en = NewEnumerator();
    if (!en) return id;
    IMMDevice* d = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d)))
    {
        LPWSTR p = nullptr;
        if (SUCCEEDED(d->GetId(&p))) { id = p; CoTaskMemFree(p); }
        d->Release();
    }
    en->Release();
    return id;
}

bool OutputAudio::SetDefaultRender(const std::wstring& id)
{
    if (!g_policy)
    {
        if (FAILED(CoCreateInstance(kCLSID_PolicyConfigClient, nullptr, CLSCTX_ALL,
                                    kIID_IPolicyConfig, (void**)&g_policy)))
            return false;
    }
    bool ok = true;
    for (int role = 0; role < 3; role++)
        if (FAILED(g_policy->SetDefaultEndpoint(id.c_str(), (ERole)role))) ok = false;
    return ok;
}

// 遍历默认输出上的音频会话，对匹配 instanceId 的会话执行操作
template <typename Fn>
static bool ForEachSession(const std::wstring& instanceId, Fn&& fn)
{
    auto defId = OutputAudio().DefaultRenderId();
    if (defId.empty()) return false;

    IMMDeviceEnumerator* en = NewEnumerator();
    if (!en) return false;

    bool done = false;
    IMMDevice* d = nullptr;
    if (SUCCEEDED(en->GetDevice(defId.c_str(), &d)))
    {
        IAudioSessionManager2* mgr = nullptr;
        if (SUCCEEDED(d->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)))
        {
            IAudioSessionEnumerator* se = nullptr;
            if (SUCCEEDED(mgr->GetSessionEnumerator(&se)))
            {
                int n = 0;
                se->GetCount(&n);
                for (int i = 0; i < n && !done; i++)
                {
                    IAudioSessionControl* c = nullptr;
                    if (FAILED(se->GetSession(i, &c))) continue;

                    IAudioSessionControl2* c2 = nullptr;
                    if (SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&c2)))
                    {
                        LPWSTR inst = nullptr;
                        if (SUCCEEDED(c2->GetSessionInstanceIdentifier(&inst)) && inst)
                        {
                            if (instanceId == inst) done = fn(c);
                            CoTaskMemFree(inst);
                        }
                        c2->Release();
                    }
                    c->Release();
                }
                se->Release();
            }
            mgr->Release();
        }
        d->Release();
    }
    en->Release();
    return done;
}

// 枚举默认输出设备上的全部音频会话，带 pid / 显示名 / 蓝牙标记
std::vector<PhoneSessionInfo> OutputAudio::AllSessions()
{
    std::vector<PhoneSessionInfo> out;

    auto defId = DefaultRenderId();
    if (defId.empty()) return out;

    IMMDeviceEnumerator* en = NewEnumerator();
    if (!en) return out;

    IMMDevice* d = nullptr;
    if (SUCCEEDED(en->GetDevice(defId.c_str(), &d)))
    {
        IAudioSessionManager2* mgr = nullptr;
        if (SUCCEEDED(d->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)))
        {
            IAudioSessionEnumerator* se = nullptr;
            if (SUCCEEDED(mgr->GetSessionEnumerator(&se)))
            {
                int n = 0;
                se->GetCount(&n);
                for (int i = 0; i < n; i++)
                {
                    IAudioSessionControl* c = nullptr;
                    if (FAILED(se->GetSession(i, &c))) continue;

                    IAudioSessionControl2* c2 = nullptr;
                    if (FAILED(c->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&c2)))
                    { c->Release(); continue; }

                    PhoneSessionInfo info;
                    info.found = true;

                    DWORD pid = 0;
                    if (SUCCEEDED(c2->GetProcessId(&pid))) info.pid = pid;

                    LPWSTR nm = nullptr;
                    if (SUCCEEDED(c2->GetDisplayName(&nm)) && nm)
                    {
                        info.displayName = nm;
                        CoTaskMemFree(nm);
                    }

                    LPWSTR inst = nullptr;
                    if (SUCCEEDED(c2->GetSessionInstanceIdentifier(&inst)) && inst)
                    {
                        info.instanceId = inst;
                        CoTaskMemFree(inst);
                    }

                    ISimpleAudioVolume* sv = nullptr;
                    if (SUCCEEDED(c2->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&sv)))
                    {
                        sv->GetMasterVolume(&info.volume);
                        BOOL m = FALSE;
                        sv->GetMute(&m);
                        info.muted = (m != FALSE);
                        sv->Release();
                    }

                    // 峰值表判断是否真的在出声
                    // 注意：AudioSessionState 会一直停留在 Active，不能用来判断播放
                    {
                        IAudioMeterInformation* mi = nullptr;
                        if (SUCCEEDED(c2->QueryInterface(__uuidof(IAudioMeterInformation), (void**)&mi)))
                        {
                            mi->GetPeakValue(&info.peak);
                            mi->Release();
                        }
                    }

                    AudioSessionState st = AudioSessionStateInactive;
                    c->GetState(&st);
                    // 峰值表受会话音量影响，安静段容易掉到阈值以下。
                    // 用「进入 0.002 / 退出 0.0004」的滞回，避免状态点忽明忽暗、
                    // 以及「正在播放却被判空闲 → 重启通道 → 可听断音」。
                    if (info.peak >= 0.f)
                    {
                        // 键是 WASAPI 会话实例 ID（含每次新建的 GUID）。scrcpy 每次重启
                        // （换缓冲/空闲重置/自愈）都产生新 ID，条目只增不删会无界增长。
                        static std::map<std::wstring, bool> lastState;
                        auto itS = lastState.find(info.instanceId);
                        bool was = (itS != lastState.end()) && itS->second;
                        bool now2 = was ? (info.peak > 0.0004f) : (info.peak > 0.002f);
                        lastState[info.instanceId] = now2;
                        info.playing = now2;
                        // 上界：超过 64 条就清空（只会影响滞回的一帧，远好于无界增长）
                        if (lastState.size() > 64) lastState.clear();
                    }
                    else info.playing = (st == AudioSessionStateActive);

                    // 蓝牙 A2DP 接收会话：显示名里带 "A2DP SNK"
                    if (info.displayName.find(L"A2DP") != std::wstring::npos)
                        info.isBt = true;

                    out.push_back(info);
                    c2->Release();
                    c->Release();
                }
                se->Release();
            }
            mgr->Release();
        }
        d->Release();
    }
    en->Release();
    return out;
}

bool OutputAudio::SetSessionVolume(const std::wstring& instanceId, float vol)
{
    return ForEachSession(instanceId, [vol](IAudioSessionControl* c)
    {
        ISimpleAudioVolume* sv = nullptr;
        bool ok = false;
        if (SUCCEEDED(c->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&sv)))
        {
            ok = SUCCEEDED(sv->SetMasterVolume(vol, nullptr));
            sv->Release();
        }
        return ok;
    });
}

bool OutputAudio::SetSessionMute(const std::wstring& instanceId, bool mute)
{
    return ForEachSession(instanceId, [mute](IAudioSessionControl* c)
    {
        ISimpleAudioVolume* sv = nullptr;
        bool ok = false;
        if (SUCCEEDED(c->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&sv)))
        {
            ok = SUCCEEDED(sv->SetMute(mute, nullptr));
            sv->Release();
        }
        return ok;
    });
}
