// WaveTether native —— Win32 + GDI+ 手绘 UI + WinRT/WASAPI 音频
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <map>
#include <set>

#include "theme.h"
#include "audio.h"
#include "adb.h"
#include "perf.h"
#include "jsonmini.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace Gdiplus;

// ------------------------------------------------------------------ 常量

static const int WM_TRAY = WM_APP + 1;
static const int WM_SHOWME = WM_APP + 2;
static const int TRAY_ID = 1;
static const wchar_t* kClassName = L"WaveTetherNative";
static const wchar_t* kSingleInstanceEvent = L"WaveTether.Native.Show";

enum : int
{
    ID_NONE = 0,
    ID_TITLE_THEME, ID_TITLE_MIN, ID_TITLE_CLOSE,
    ID_BTN_CONNECT, ID_BTN_REFRESH, ID_BTN_DELETE,
    ID_BT_OPEN_SETTINGS, ID_PAIR_OPEN, ID_PAIR_FIND, ID_PAIR_ADDR, ID_PAIR_CODE, ID_PAIR_PORT, ID_PAIR_GO, ID_PAIR_CANCEL,
    ID_BT_LIST, ID_PRIO_LIST, ID_ADB_ROW, ID_BT_UPGRADE,
    ID_SLIDER, ID_MUTE, ID_VOL_GLOBAL, ID_VOL_SINGLE, ID_DEV_SELECT,
    ID_FIXED_OUT,
    ID_MOVE_UP, ID_MOVE_DOWN, ID_REFRESH_OUT, ID_SOUND_SETTINGS,
    ID_AUTOSTART, ID_AUTOCONNECT, ID_SETTINGS, ID_LOG, ID_QUIT,
    ID_SET_CLOSE_TRAY, ID_SET_CLOSE_EXIT, ID_SET_MIN_TRAY, ID_SET_MIN_TASKBAR,
    ID_SET_LANSCAN, ID_SET_AUTORECONN, ID_SET_AUTOADAPT, ID_SET_SILENT, ID_SET_BODY,
    ID_SET_IDLERESET, ID_SET_DUAL, ID_SET_AUTODUAL, ID_SET_AUTOUP, ID_WIZ_CLOSE, ID_WIZ_HELP,ID_DEV_AUTO,
    ID_SET_GEAR_BASE, ID_SET_GEAR_1 = ID_SET_GEAR_BASE + 1, ID_SET_GEAR_2 = ID_SET_GEAR_BASE + 2, ID_SET_GEAR_3 = ID_SET_GEAR_BASE + 3,
    ID_SET_DONE,
    ID_TAB_BT, ID_TAB_ADB,
    ID_HELP, ID_HELP_CLOSE, ID_REFRESH, ID_CHECK,
    ID_HELP_NAV_CHANNEL, ID_HELP_NAV_PAIR, ID_HELP_NAV_VOLUME, ID_HELP_NAV_OUTPUT, ID_HELP_NAV_FIXED,
    ID_HELP_NAV_MULTI, ID_HELP_NAV_STATUS, ID_HELP_NAV_TROUBLE, ID_HELP_NAV_WHY, ID_HELP_NAV_RUNTIME, ID_HELP_BODY,
    ID_HELP_Q_VOLUME, ID_HELP_Q_OUTPUT, };

struct RECTF : Gdiplus::RectF
{
    RECTF() : Gdiplus::RectF(0, 0, 0, 0) {}
    RECTF(float x, float y, float w, float h) : Gdiplus::RectF(x, y, w, h) {}
    RECTF(const Gdiplus::RectF& o) : Gdiplus::RectF(o.X, o.Y, o.Width, o.Height) {}
    float GetBottom() const { return Y + Height; }
    float GetRight()  const { return X + Width; }
    bool Contains(float px, float py) const
    {
        return px >= X && px <= X + Width && py >= Y && py <= Y + Height;
    }
};

struct Hit { int id; int index; RECTF r; };

// 前置声明（定义在后面，但 App 内的方法要用）
static std::wstring Utf8ToWide(const std::string& s);
static std::string WideToUtf8(const std::wstring& w);
struct RowItem
{
    std::wstring name; std::wstring sub; bool ok;
    std::string serial; std::string identity;
    bool running = false; bool devPlaying = false;
    int  btState = 0;      // 0=未连接 1=连接中 2=已连接（蓝牙链路，仅蓝牙页用）
    bool btAudio = false;  // 已连接时：音频是否真的走蓝牙
    std::string btMac;     // 蓝牙 MAC（身份，不随改名变化）
    std::wstring btId;     // WinRT 设备 Id（未配对的历史记录为空，不能连）
    bool btPaired = true;  // false = 只在历史里、当前没枚举到
    bool btOnlyAdb = false; // true = 只在 ADB 见过，从没蓝牙配对过
    bool btEmptyHint = false; // true = 「还没有配对设备」的占位行，点了打开 Windows 蓝牙设置
    bool adbOffline = false;  // true = 认识这台设备，但当前不在 adb devices 里
    std::string adbIdentity;  // ADB 设备身份（ro.serialno），音量键要用它而不是 serial
};
static std::vector<RowItem> CurrentRows();




// ------------------------------------------------------------------ 状态

#include "version.h"
#include "pure.h"

// ---------------- 帮助 ----------------
// 版本号只在 version.h 里定义一次，这里只是换成宽字符
#define APP_VERSION      L"" APP_VERSION_A
#define APP_VERSION_FULL L"" APP_VERSION_4A

enum HelpTopic { HELP_NONE = 0, HELP_CHANNEL, HELP_PAIR, HELP_VOLUME, HELP_OUTPUT, HELP_FIXED, HELP_MULTI, HELP_STATUS, HELP_TROUBLE, HELP_WHY, HELP_RUNTIME };

struct App
{
    ULONGLONG perfClickAt = 0;      // 临时诊断：最近一次点击的时间
    Theme th = LightTheme();   // 默认浅色，和 settings 里 themeLight=true 保持一致
    HDC memDC = nullptr;
    HBITMAP memBmp = nullptr;
    int memW = 0, memH = 0;
    float dpi = 1.f;

    BtLink bt;
    OutputAudio oa;
    AdbChannel adb;

    std::vector<SinkDevice> btDevs;
    int btSel = -1;
    std::vector<OutDevice> outs;
    std::vector<std::wstring> prio;      // 优先顺序的端点 id
    std::vector<bool> prioOn;            // 是否勾选（与 prio 同序）
    int prioSel = -1;
    std::vector<std::wstring> prioOrder; // 全量（含未勾选）

    PhoneSessionInfo session;
    float phoneVol = 0.5f;    // 滑块 = 手机那一路的会话音量（不碰系统全局音量）
    bool phoneMute = false;   // 仅静音手机那一路
    std::vector<PhoneSessionInfo> phoneSessions;   // 多设备：每台一个会话

    // ---- 逐设备音量 ----
    struct DevVolEntry
    {
        std::wstring key;          // "adb:<serial>" 或 "bt:<名称>"
        std::wstring label;        // 显示名
        bool isBt = false;
        bool playing = false;
        float vol = 1.f;
        bool mute = false;
        std::wstring instanceId;
    };
    std::vector<DevVolEntry> devVols;                 // 当前可调的设备音量（每帧刷新）
    std::map<std::wstring, float> devVolPref;         // 持久化：设备 → 音量
    std::map<std::wstring, bool>  devMutePref;        // 持久化：设备 → 静音

    // 蓝牙设备历史：MAC → 当前名字。MAC 不变，名字会变，所以键用 MAC。
    // 蓝牙页 = 这份历史 ∪ 当前枚举结果，按 MAC 去重 —— 这样「连接过的设备」都会一直显示。
    std::map<std::string, std::wstring> btHistory;
    bool btHistoryDirty = false;
    ULONGLONG deleteArmedUntil = 0;   // 「删除」二次确认的截止时间

    // 无线调试配对对话框（Android 11+）
    HIMC savedImeCtx = nullptr;   // 打开配对框前的输入法上下文，关闭时恢复
    bool pairOpen = false;
    int  pairFocus = 0;                        // 0=地址 1=配对码 2=连接端口
    std::wstring pairAddr, pairCode, pairPort;
    std::wstring pairStatusLocal;              // 本地即时反馈（点「配对」后立刻显示）
    RECTF noticeBtnRect;                       // 标题栏提示条里动作按钮的位置（NCHITTEST 用）
    int volMode = 1;                                  // 0=全局  1=单个（默认单个，直接调对这台）
    int adbSel = 0;                                   // ADB 列表选中项
    bool volSelInit = false;                          // 启动时是否已自动选中过

    // 设备的音量/静音键。
    // **必须用 identity（ro.serialno）**，不能用 serial：
    // 同一台手机可能以 ip:5555、随机端口、或 mDNS 名字三种 serial 出现，
    // 用 serial 做键会变成三条互不相干的记录，换条链路音量就不跟着走。
    static std::wstring AdbVolKey(const AdbDevice& d)
    {
        return L"adb:" + Utf8ToWide(d.identity.empty() ? d.serial : d.identity);
    }

    // 蓝牙的音量/静音键：MAC 优先。
    // 用名字做键的话，设备一改名，之前调好的音量就“丢了”（其实还在，只是对不上）。
    static std::wstring BtVolKey(const std::wstring& name, const std::string& mac)
    {
        return mac.empty() ? (L"bt:" + name) : (L"bt:" + Utf8ToWide(mac));
    }

    // 当前左侧选中的设备 key
    std::wstring SelectedKey() const
    {
        if (useAdb)
        {
            auto u = adb.UiUniqueDevices();   // 必须用快照版：UniqueDevices() 会抢 mtx_，工作线程做 adb IO 时一抢就是好几秒
            if (adbSel >= 0 && adbSel < (int)u.size())
                return AdbVolKey(u[adbSel]);
            return {};
        }
        // 蓝牙页的列表是「历史 ∪ 枚举 ∪ 仅 ADB」合并后的，索引不能拿去查 btDevs
        auto rows = CurrentRows();
        if (btSel < 0 || btSel >= (int)rows.size()) return {};
        auto& r = rows[btSel];

        // 这台如果正在通过 ADB 转发，音量要调 ADB 那条会话。
        // 否则会出现「设备在播、界面却说未转发、滑块禁用」的自相矛盾。
        for (auto& d : adb.UiUniqueDevices())
        {
            if (d.state != "device") continue;
            bool match = (!r.btMac.empty() && !d.btMac.empty() && d.btMac == r.btMac)
                      || (!d.btName.empty() && Utf8ToWide(d.btName) == r.name);
            if (match && adb.UiIsRunning(d.serial))
                return AdbVolKey(d);
        }
        return BtVolKey(r.name, r.btMac);
    }

    // 画面签名：内容没变就不重绘（原来每 250ms 无条件全量重绘）
    size_t VisualSig()
    {
        size_t h = 1469598103934665603ull;
        auto mix = [&](long long v) { h ^= (size_t)v; h *= 1099511628211ull; };
        auto mixS = [&](const std::wstring& str) { for (wchar_t c : str) { h ^= (size_t)c; h *= 1099511628211ull; } };

        mix(useAdb); mix(volMode); mix(btSel); mix(adbSel);
        mix((int)bt.State()); mix((int)adb.State()); mix(adb.UiRunningCount());
        mix((int)(phoneVol * 1000)); mix(phoneMute); mix(fixedMode);
        mix(helpOpen); mix(helpTopic); mix(settingsOpen); mix(showWizard);
        mix((int)helpScroll);
        mixS(GetAdbHint()); mixS(GetNoticeText()); mix((int)GetNoticeLevel());
        mixS(currentDefaultId); mixS(currentDefaultName);
        mixS(GetNoticeText()); mix(GetNoticeLevel()); mix((int)(GetTickCount64() < GetNoticeUntil()));
        for (auto& b : btDevs) mixS(b.name);
        mix(routed); mix(themeLight); mix(hover); mix(hoverIdx);

        for (auto& e : devVols)
        { mixS(e.key); mix(e.playing); mix((int)(e.vol * 1000)); mix(e.mute); mix(e.isBt); }

        for (auto& d : btDevs) mixS(d.name);

        for (auto& r : CurrentRows())
        { mixS(r.name); mixS(r.sub); mix(r.ok); mix(r.running); mix(r.devPlaying); }

        mix((long long)outs.size()); mix((long long)prio.size());
        return h;
    }

    // 统一的播放/连接状态文本（标题栏与音量卡共用同一套口径）
    struct StatusInfo { std::wstring text; COLORREF color; bool playing = false; };

    StatusInfo MakeStatus()
    {
        StatusInfo r;
        if (volMode == 1)
        {
            auto* e = SelectedEntry();
            if (!e) { r.text = L"该设备未转发"; r.color = th.TextFaint; return r; }
            r.playing = e->playing;
            if (e->mute)        { r.text = L"已静音";     r.color = th.Ok; }
            else if (e->playing){ r.text = L"● 正在播放"; r.color = th.AccentText; }  // 蓝点
            else                { r.text = L"无播放";     r.color = th.Ok; }           // 绿点
            return r;
        }

        // 状态反映「实际在播的链路」，不跟随当前标签页：
        // 切到蓝牙页看列表时，ADB 转发仍在继续，状态不该变成「未连接」
        int devN = adb.UiRunningCount();   // 必须读快照：RunningCount() 会抢工作线程的 mtx_
        bool btUp = (bt.State() == LinkState::Connected);
        r.playing = (devN > 0) ? (phonePlaying != 0) : (session.found && session.playing);

        if (phoneMute)          { r.text = L"已静音"; r.color = th.Ok; }
        else if (r.playing)     { r.text = (devN > 1) ? (std::to_wstring(devN) + L" 台设备 · ● 正在播放")
                                                      : std::wstring(L"● 正在播放");
                                  r.color = th.AccentText; }                       // 蓝点
        else if (devN > 0)      { r.text = (devN > 1) ? (std::to_wstring(devN) + L" 台设备 · 无播放")
                                                      : std::wstring(L"ADB 已连接 · 无播放");
                                  r.color = th.Ok; }                               // 绿点
        else if (btUp)          { r.text = L"已连接 · 无播放"; r.color = th.Ok; }   // 绿点
        else if (session.found) { r.text = L"已连接 · 无播放"; r.color = th.Ok; }   // 绿点
        else                    { r.text = L"无活动"; r.color = th.TextFaint; }     // 灰点
        return r;
    }

    // 选中设备当前是否有可调的音量会话（未转发时为 null）
    DevVolEntry* SelectedEntry()
    {
        auto k = SelectedKey();
        if (k.empty()) return nullptr;
        return FindDevVol(k);
    }

    DevVolEntry* FindDevVol(const std::wstring& key)
    {
        for (auto& e : devVols) if (e.key == key) return &e;
        return nullptr;
    }

    // ---- 全局 / 单个 模式的读写 ----
    float ActiveVolValue()
    {
        if (volMode == 0) return phoneVol;
        auto k = SelectedKey();
        if (k.empty()) return phoneVol;
        auto it = devVolPref.find(k);
        if (it != devVolPref.end()) return it->second;
        for (auto& e : devVols) if (e.key == k) return e.vol;   // 首次：用会话当前值
        return phoneVol;
    }
    bool ActiveMuteValue()
    {
        if (volMode == 0) return phoneMute;
        auto k = SelectedKey();
        if (k.empty()) return phoneMute;
        auto it = devMutePref.find(k);
        if (it != devMutePref.end()) return it->second;
        for (auto& e : devVols) if (e.key == k) return e.mute;
        return phoneMute;
    }
    // 音量一律量化到整百分比：内部还是 0..1 浮点（WASAPI 需要），
    // 但取值只有 0%、1%、…、100% 这 101 档，不会再出现 0.102499 这种小数。
    static float QuantizeVol(float v)
    {
        if (v < 0.f) v = 0.f;
        if (v > 1.f) v = 1.f;
        return (float)((int)(v * 100.f + 0.5f)) / 100.f;
    }

    void SetActiveVol(float v)
    {
        v = QuantizeVol(v);
        if (volMode == 0) { phoneVol = v; return; }
        auto k = SelectedKey();
        if (!k.empty()) devVolPref[k] = v;
    }
    void SetActiveMute(bool m)
    {
        if (volMode == 0) { phoneMute = m; return; }
        auto k = SelectedKey();
        if (!k.empty()) devMutePref[k] = m;
    }

    // ---- 运行状态（这些字段原本在音量块之后，重构时被误删，此处补回）----
    bool useAdb = false;
    bool themeLight = true;    // 默认浅色
    // 默认关：连接时自动切系统默认输出。
    // 之所以默认关：切换是整机生效的（Windows 不支持按程序分设备），
    // 一旦程序异常退出没还原，用户会「整机没声音且找不到原因」——
    // 而他会去修电脑，不会想到是这个软件。要自动切的人自己勾。
    bool fixedMode = false;
    // adbHint 会被工作线程（Post 的 lambda）写、UI 线程读，std::wstring 不能裸并发
    std::wstring adbHint;
    mutable std::mutex hintMtx_;
    void SetAdbHint(const std::wstring& h) { std::lock_guard<std::mutex> lk(hintMtx_); adbHint = h; }
    std::wstring GetAdbHint() const { std::lock_guard<std::mutex> lk(hintMtx_); return adbHint; }

    std::wstring currentDefaultId, currentDefaultName;
    bool routed = false;
    std::wstring routedPrev;
    // 崩溃后还原默认输出用：routeDirty = 当前处于「我们把默认设备切走了、还没还原」。
    // 只靠这两个条件还原：① 上次脏退出 ② 当前默认 == routeTarget。
    std::wstring routeTarget;
    std::wstring routePrev;
    bool routeDirty = false;

    std::vector<Hit> hits;
    int hover = ID_NONE, hoverIdx = -1;
    int pressed = ID_NONE, pressedIdx = -1;
    int scrollDev = 0, scrollPrio = 0;

    bool dragSlider = false;

    bool settingsOpen = false;
    bool helpOpen = false;
    int helpTopic = HELP_CHANNEL;
    float helpScroll = 0.f;

    NOTIFYICONDATAW nid{};
    bool trayAdded = false;

    // 把音量/静音应用到所有手机会话（全局模式）
    void ApplyVolumeAll(float v)
    {
        for (auto& ps : phoneSessions)
            if (ps.found) oa.SetSessionVolume(ps.instanceId, v);
    }
    void ApplyMuteAll(bool m)
    {
        for (auto& ps : phoneSessions)
            if (ps.found) oa.SetSessionMute(ps.instanceId, m);
    }

    // ADB 设备身份 -> 蓝牙名（同一台设备用蓝牙名显示）
    std::map<std::wstring, std::wstring> btNames;   // identity → 蓝牙名
    std::map<std::wstring, std::wstring> btMacs;    // identity → 蓝牙 MAC（用于并入总设备列表）
    // 「删除」过的设备（存 MAC，没有 MAC 时存 identity）。
    // 作用：只在 ADB 出现过的设备，删掉后不会被每 4 秒的同步重新学回来。
    // 用户在 ADB 页重新勾选、或点 ↑ADB 时解除。
    std::set<std::string> btDeleted;

    std::wstring NameForIdentity(const std::string& identity) const
    {
        if (identity.empty()) return {};
        auto it = btNames.find(Utf8ToWide(identity));
        return (it == btNames.end()) ? std::wstring() : it->second;
    }
    // 记住 ADB 设备的蓝牙身份（名字 + MAC）。
    // 目的：只用过 ADB、从没在 Windows 蓝牙里配对的设备，也能出现在总设备列表里。
    bool RememberAdbDevice(const std::string& identity, const std::wstring& name, const std::string& mac)
    {
        if (identity.empty()) return false;
        // 已删除的设备不再自动学回来（否则删完 4 秒又出现）
        if (btDeleted.count(identity)) return false;
        if (!mac.empty() && btDeleted.count(mac)) return false;
        bool changed = false;
        std::wstring key = Utf8ToWide(identity);
        if (!name.empty())
        {
            auto& slot = btNames[key];
            if (slot != name) { slot = name; changed = true; }
        }
        if (!mac.empty())
        {
            std::wstring m = Utf8ToWide(mac);
            auto& slot = btMacs[key];
            if (slot != m) { slot = m; changed = true; }
        }
        return changed;
    }

    bool RememberBtName(const std::string& identity, const std::wstring& name)
    {
        if (identity.empty() || name.empty()) return false;
        auto& slot = btNames[Utf8ToWide(identity)];
        if (slot == name) return false;      // 没变就不写盘
        slot = name;
        return true;
    }
    int adbTick = 0;           // 节流：每 N 次 tick 探测一次
    int adoptTick = 0;         // 节流：接管外部 scrcpy 的检查频率
    volatile LONG phonePlaying = 0; // 手机端是否真有音频在播（后台线程写，UI 读）
    int btScanTick = 0;        // 蓝牙列表刷新节流（16*250ms = 4s）

    std::wstring settingsPath;
    std::wstring lastDeviceId, lastDeviceName;
    bool autoStart = false, autoConnect = true;
    bool silentStart = true;   // 开机自启后是否静默进托盘
    bool idleReset = true;     // 缓冲变更/重建是否只在空闲（没在播放）时做
    bool autoUpgrade = true;   // 蓝牙连上后是否自动升级到 ADB 音频（手动按钮不受此开关影响）
    bool autoReconnect = true; // 运行期间设备掉线是否自动连回来（关掉 = 开关 WiFi 即断开）
    bool showWizard = false;   // 首次设置向导
    bool setupDone = false;    // 是否已完成过首次设置
    // 工作线程不能直接操作 BtLink（WinRT 对象是 STA 亲和的，跨线程会崩）。
    // 这里只置请求，真正的断开在 UI 线程的定时器里做。
    std::atomic<bool> btDisconnectRequest{ false };
    std::set<std::string> userStopped;   // 用户手动点了 ■ 的设备，不再自动启动
    std::set<std::string> autoDevices;   // 勾选「自动连接」的设备（存 identity，IP 变化不影响）

    // SetNotice 会被工作线程调用（Post 的 lambda 里），UI 线程每 250ms 读。
    // std::wstring 不能裸并发（赋值/读取并发是未定义行为，可能堆损坏），所以加锁。
    void SetNotice(const std::wstring& t, int level, DWORD ms = 8000)
    {
        std::lock_guard<std::mutex> lk(noticeMtx_);
        noticeText = t; noticeLevel = level; noticeUntil = GetTickCount64() + ms;
    }
    std::wstring GetNoticeText() const
    {
        std::lock_guard<std::mutex> lk(noticeMtx_);
        return noticeText;
    }
    int GetNoticeLevel() const { std::lock_guard<std::mutex> lk(noticeMtx_); return noticeLevel; }
    ULONGLONG GetNoticeUntil() const { std::lock_guard<std::mutex> lk(noticeMtx_); return noticeUntil; }

    // 顶部提示条（把关键问题从「淡灰小字」变成「醒目横幅」）
    mutable std::mutex noticeMtx_;
    std::wstring noticeText;
    int  noticeLevel = 0;          // 0=信息(蓝) 1=警告(黄) 2=错误(红)
    ULONGLONG noticeUntil = 0;     // 临时提示的过期时间（用 64 位，避免 49.7 天回绕后提示条永久卡住）

    // 这台设备是否被勾选（优先按 identity，兼容旧版本按 serial 记录）
    bool IsAutoDevice(const RowItem& it) const
    {
        if (!it.identity.empty() && autoDevices.count(it.identity)) return true;
        return autoDevices.count(it.serial) > 0;   // 旧数据兼容
    }

    // 蓝牙设备枚举放到后台：FindAllAsync 会阻塞 200~800ms，在 UI 线程会卡顿
    std::mutex btScanMtx_;
    std::vector<SinkDevice> btScanResult_;
    std::atomic<bool> btScanReady_{ false };
    std::atomic<bool> btScanBusy_{ false };
    float settingsScroll = 0.f;
    RECTF settingsBody{};
    HBITMAP oldBmp = nullptr;

    // 静音自愈：{serial → {本次会话启动时刻, 是否已尝试过, 是否曾出过声}}
    struct HealState
    {
        DWORD spawnMs = 0;
        DWORD silentSince = 0;    // 从什么时候开始完全没声音（0 = 有声音）
        DWORD lastNudge = 0;      // 上次自愈时刻（限流）
        bool  diagDone = false;   // 本轮静音是否已经记过现场
    };
    std::map<std::string, HealState> healState;
    float settingsContentH = 0.f;   // 上一帧内容总高，用于提前夹紧滚动
    std::wstring closeAction = L"tray", minAction = L"taskbar";   // 最小化默认回任务栏（不是收托盘）
};
// 故意不析构：App 含 COM/GDI 相关成员，若在 CoUninitialize 之后析构会崩溃
static App& g = *new App();

// 设备列表的统一视图：蓝牙模式取 btDevs，ADB 模式取 adb.Devices()

static std::vector<RowItem> CurrentRows()
{
    std::vector<RowItem> out;
    if (::g.useAdb)
    {
        // 必须用去重后的列表：手机同时开 tcpip 5555 和无线调试时，
        // 同一台设备会有 2~4 个连接入口（不同端口 / mDNS 名字）。
        // 只取一次：UiUniqueDevices 每次都要复制整个快照，重复调用白花时间。
        auto mergedDevs = ::g.adb.UiUniqueDevices();
        for (auto& d : mergedDevs)
        {
            RowItem r;
            r.adbIdentity = d.identity;
            // 名字优先用设备自己的蓝牙名，其次历史映射，再次型号，最后 serial
            std::wstring devBt = Utf8ToWide(d.btName);     // adb 输出是 UTF-8，必须解码
            std::wstring mapped = ::g.NameForIdentity(d.identity);
            if (!devBt.empty())        r.name = devBt;
            else if (!mapped.empty())  r.name = mapped;
            else if (!d.model.empty()) r.name = Utf8ToWide(d.model);
            else                       r.name = Utf8ToWide(d.serial);

            r.serial = d.serial;
            r.identity = d.identity;
            r.running = ::g.adb.UiIsRunning(d.serial);
            // 手机端是否在出声：取该设备音频会话的峰值表结果
            for (auto& e : ::g.devVols)
                if (e.key == App::AdbVolKey(d)) { r.devPlaying = e.playing; break; }
            // 无线要区分「已配对」和「老的 tcpip 5555」：
            // 前者手机重启也不掉，后者重启后必须再插一次线。以前两者长得一样，用户没法判断。
            r.sub = d.isWired ? L"USB"
                  : (d.pairedState == 1) ? L"无线 · 已配对（重启不掉）"
                  : (d.pairedState == 2) ? L"无线 · 未配对（重启要插线）"
                                          : L"无线";     // 还没识别完，不瞎猜
            r.ok = (d.state == "device");
            if (d.state == "unauthorized") r.sub = L"未授权（请在设备上允许调试）";
            else if (d.state == "offline") r.sub = L"离线";
            out.push_back(r);
        }
        // 认识但当前不在线（典型：手机重启后无线调试关了）。
        // 以前这种设备在 ADB 页完全不出现，用户只能看到「少了一台」，
        // 完全不知道是被关了调试还是掉线了。
        std::set<std::string> onlineIdent;
        for (auto& d : mergedDevs)
            if (!d.identity.empty()) onlineIdent.insert(d.identity);

        for (auto& kv : ::g.btNames)
        {
            std::string id8 = WideToUtf8(kv.first);
            if (id8.empty() || onlineIdent.count(id8)) continue;
            bool dup = false;
            for (auto& r : out) if (r.name == kv.second) { dup = true; break; }
            if (dup) continue;
            RowItem r;
            r.name = kv.second;
            r.sub = L"离线";
            r.ok = false;
            r.adbOffline = true;
            out.push_back(r);
        }

        if (out.empty())
        {
            // 蓝牙版（只有 exe，没有 tools）根本不可能有 ADB 设备，
            // 还写「插 USB 线 / 开无线调试」等于让用户白折腾。
            if (!::g.adb.HasAdb())
                out.push_back({ L"本版本不支持 ADB", L"用「蓝牙」页即可，功能不受影响", false });
            else
                out.push_back({ L"未找到 ADB 设备", L"插 USB 线，或在设备上开启无线调试", false });
        }
    }
    else
    {
        // 蓝牙页 = 历史记录 ∪ 当前枚举结果，按 MAC 去重。
        // 这样「连接过的设备」会一直留在列表里，即使这次没配对上。
        std::set<std::string> seenMac;
        for (auto& d : ::g.btDevs)
        {
            RowItem r;
            r.name = d.name;
            r.ok = true;
            r.btMac = d.mac;
            r.btId = d.id;
            r.btPaired = true;
            if (!d.mac.empty()) seenMac.insert(d.mac);

            // 蓝牙一次只连一台：只有「当前这台」才谈得上连接状态
            if (::g.bt.DeviceName() == d.name)
            {
                LinkState st = ::g.bt.State();
                if (st == LinkState::Connected)      r.btState = 2;
                else if (st == LinkState::Connecting) r.btState = 1;
                if (r.btState == 2)
                {
                    // 音频是否真走蓝牙：看这条链路的音频会话有没有峰值
                    for (auto& e : ::g.devVols)
                        if (e.key == App::BtVolKey(d.name, d.mac)) { r.btAudio = e.playing; break; }
                }
            }
            out.push_back(r);
        }

        // 历史里有、但这次没枚举到的：可能已取消配对，也可能只是不在范围。
        // 只展示不删除 —— 删不删由用户用「删除」决定，不替他做主。
        std::set<std::wstring> seenName;
        for (auto& r : out) seenName.insert(r.name);
        for (auto& kv : ::g.btHistory)
        {
            if (seenMac.count(kv.first)) continue;
            if (seenName.count(kv.second)) continue;
            RowItem r;
            r.name = kv.second;
            r.ok = false;
            r.btMac = kv.first;
            r.btPaired = false;
            seenName.insert(kv.second);      // 关键：否则下面的 btNames 会把同一台再加一遍
            if (!kv.first.empty()) seenMac.insert(kv.first);
            out.push_back(r);
        }

        // ADB 见过、但 Windows 蓝牙里从没配对过的设备。
        // 这就是「只用过 ADB 的设备」以前在蓝牙页完全不显示的原因。
        for (auto& kv : ::g.btNames)
        {
            if (kv.second.empty()) continue;
            if (seenName.count(kv.second)) continue;
            {
                std::string id8 = WideToUtf8(kv.first);
                if (::g.btDeleted.count(id8)) continue;
            }
            std::string mac;
            auto itm = ::g.btMacs.find(kv.first);
            if (itm != ::g.btMacs.end())
            {
                std::string m = WideToUtf8(itm->second);
                if (seenMac.count(m)) continue;
                if (::g.btDeleted.count(m)) continue;
                mac = m;
            }
            RowItem r;
            r.name = kv.second;
            r.ok = true;
            r.btMac = mac;
            r.btPaired = false;      // 没配对 → 不能连蓝牙
            r.btOnlyAdb = true;
            seenName.insert(kv.second);
            if (!mac.empty()) seenMac.insert(mac);   // 同一台设备的两条 identity 不再重复出两行
            out.push_back(r);
        }

        // 一台都没有 → 给个能点的空态。ADB 页一直有这个，蓝牙页原来是一片空白，
        // 而「手机还没配对」正是首次使用必经的第一个状态。
        if (out.empty())
        {
            RowItem r;
            r.name = L"还没有配对的设备";
            r.sub = L"点这里打开 Windows 蓝牙设置";
            r.ok = false;
            r.btEmptyHint = true;
            out.push_back(r);
        }
    }
    return out;
}




static HWND g_hwnd = nullptr;
static ULONG_PTR g_gdiToken = 0;

// ------------------------------------------------------------------ 工具

static Color C(COLORREF c, BYTE a = 255)
{
    return Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

static void MakeRoundRect(GraphicsPath& p, float x, float y, float w, float h, float r)
{
    if (w <= 0 || h <= 0) return;
    if (r <= 0.1f) { p.AddRectangle(RectF(x, y, w, h)); return; }
    float d = r * 2;
    if (d > w) d = w;
    if (d > h) d = h;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

static void FillRound(Graphics& g, RECTF r, float rad, COLORREF c, BYTE a = 255)
{
    GraphicsPath p;
    MakeRoundRect(p, r.X, r.Y, r.Width, r.Height, rad);
    SolidBrush b(C(c, a));
    g.FillPath(&b, &p);
}

static void StrokeRound(Graphics& g, RECTF r, float rad, COLORREF c, float th, BYTE a = 255)
{
    GraphicsPath p;
    MakeRoundRect(p, r.X, r.Y, r.Width, r.Height, rad);
    Pen pen(C(c, a), th);
    g.DrawPath(&pen, &p);
}

// 字体缓存：避免每帧新建 GDI+ Font/FontFamily（内存与 CPU 抖动的主要来源）
// 注意：这两个必须是「永不析构」的堆对象。
// 函数内 static 会在进程退出时析构，而那时 GdiplusShutdown() 已经调用过，
// 对已关闭的 GDI+ 释放对象会直接崩溃。
static FontFamily& FamSemibold()
{
    static FontFamily* f = new FontFamily(L"Segoe UI Semibold");
    return *f;
}
static FontFamily& FamRegular()
{
    static FontFamily* f = new FontFamily(L"Segoe UI");
    return *f;
}
static Font& GetFont(float px, bool bold, bool regular = false)
{
    static std::map<int, Font*> cache;
    int key = (int)(px * 10.f) * 4 + (regular ? 2 : 0) + (bold ? 1 : 0);
    auto it = cache.find(key);
    if (it != cache.end()) return *it->second;

    FontFamily* fam = regular ? &FamRegular() : &FamSemibold();
    Font* f = nullptr;
    { PerfScope ps("newfont", 1.0, (int)px); f = new Font(fam, px, bold ? FontStyleBold : FontStyleRegular, UnitPixel); }
    cache[key] = f;
    return *f;
}

// 注意：bold 参数**不生效**。全部文字统一用 Segoe UI Semibold
// （Regular 在小字号 + 灰阶抗锯齿下笔画过细，费眼）。参数保留只为不动 40+ 调用点。
static void Text(Graphics& g, const std::wstring& s, float x, float y, float px,
                 COLORREF c, bool /*bold*/ = false, float width = -1.f, bool right = false)
{
    Font& f = GetFont(px, false);
    SolidBrush br(C(c));
    StringFormat sf;
    sf.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    if (right) { sf.SetAlignment(StringAlignmentFar); }
    if (width > 0)
        g.DrawString(s.c_str(), (INT)s.size(), &f, RectF(x, y, width, px * 1.8f), &sf, &br);
    else
        g.DrawString(s.c_str(), (INT)s.size(), &f, PointF(x, y), &sf, &br);
}

static void TextCenter(Graphics& g, const std::wstring& s, RECTF r, float px,
                       COLORREF c, bool /*bold*/ = false)
{
    Font& f = GetFont(px, false);
    SolidBrush br(C(c));
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(s.c_str(), (INT)s.size(), &f, r, &sf, &br);
}

static void TextWrap(Graphics& g, const std::wstring& s, float x, float y, float w, float h,
                     float px, COLORREF c)
{
    Font& f = GetFont(px, false);
    SolidBrush br(C(c));
    StringFormat sf;
    g.DrawString(s.c_str(), (INT)s.size(), &f, RectF(x, y, w, h), &sf, &br);
}

// 逐段测量换行后的总高度：与 DrawString 的换行行为一致
static float MeasureWrapped(Graphics& g, const std::wstring& s, float w, float px)
{
    Font& f = GetFont(px, false);
    StringFormat sf;
    float total = 0.f;
    size_t start = 0;
    while (start <= s.size())
    {
        size_t nl = s.find(L'\n', start);
        std::wstring line = s.substr(start, (nl == std::wstring::npos) ? std::wstring::npos : nl - start);
        if (line.empty())
        {
            RectF probe;
            g.MeasureString(L" ", 1, &f, PointF(0, 0), &sf, &probe);
            total += probe.Height;
        }
        else
        {
            RectF box;
            // 必须用 RectF 重载：SizeF 重载不做自动换行，会严重低估高度
            g.MeasureString(line.c_str(), (INT)line.size(), &f, RectF(0, 0, w, 6000.f), &sf, &box);
            total += box.Height;
        }
        if (nl == std::wstring::npos) break;
        start = nl + 1;
    }
    return total;
}

static float TextWidth(Graphics& g, const std::wstring& s, float px, bool /*bold*/ = false)
{
    Font& f = GetFont(px, false);
    RectF box;
    StringFormat sf;
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    g.MeasureString(s.c_str(), (INT)s.size(), &f, PointF(0, 0), &sf, &box);
    return box.Width;
}

// ------------------------------------------------------------------ 设置读写（极简 JSON）

static void EnsureSettingsPath()
{
    wchar_t* appdata = nullptr;
    size_t n = 0;
    _wdupenv_s(&appdata, &n, L"APPDATA");
    std::wstring dir = appdata ? appdata : L".";
    free(appdata);
    dir += L"\\WaveTether";
    CreateDirectoryW(dir.c_str(), nullptr);
    ::g.settingsPath = dir + L"\\settings.json";
}

static std::string ReadAll(const std::wstring& path)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
    {
        PerfLog("SETTINGS 读取失败（错误码 %lu）", GetLastError());
        return {};
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string s((size_t)(len > 0 ? len : 0), 0);
    if (len > 0)
    {
        size_t got = fread(&s[0], 1, (size_t)len, f);
        s.resize(got);
    }
    fclose(f);
    return s;
}

// 原子写：先写 .tmp 再替换。
// 直接覆盖原文件时，写盘过程中断电/崩溃/被杀会留下 0 字节或半截文件
// （实测抓到过 size=0），下次启动就会读成默认值并把默认值写死 —— 用户配置永久丢失。
// 判定函数 LooksLikeJson / PickSettingsText 在 pure.h（那里能被单测覆盖）。

static void WriteAll(const std::wstring& path, const std::string& s)
{
    std::wstring tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f)
    {
        PerfLog("SETTINGS 写入失败：无法创建临时文件（错误码 %lu）", GetLastError());
        return;
    }
    size_t put = fwrite(s.data(), 1, s.size(), f);
    fflush(f);
    fclose(f);
    if (put != s.size())
    {
        PerfLog("SETTINGS 写入不完整 %zu/%zu（磁盘满？）", put, s.size());
        DeleteFileW(tmp.c_str());
        return;
    }
    // 原子替换前先把上一份存成 .bak。
    // 否则一旦写到一半断电 / 磁盘满，settings.json 直接没了，
    // 用户的设备列表、音量、配对记录全部归零。
    //
    // 关键：旧文件必须是「完整的 JSON」才拿来当备份。
    // 如果它已经是被截断的半截文件，再拷过去就把唯一的好备份也覆盖了。
    {
        std::string cur = ReadAll(path);
        if (LooksLikeJson(cur))
            CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE);
    }

    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        PerfLog("SETTINGS 替换失败（错误码 %lu）", GetLastError());
        DeleteFileW(tmp.c_str());
    }
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::string JsonEscape(const std::string& in)
{
    std::string o;
    o.reserve(in.size() + 8);
    const char kBS = (char)0x5C;
    for (char ch : in)
    {
        switch (ch)
        {
        case (char)0x5C: o += kBS; o += kBS; break;
        case (char)0x22: o += kBS; o += (char)0x22; break;
        case (char)0x0A: o += kBS; o += 'n'; break;
        case (char)0x0D: o += kBS; o += 'r'; break;
        case (char)0x09: o += kBS; o += 't'; break;
        default:
            if ((unsigned char)ch < 0x20)
            {
                char b[8]; b[0] = kBS; b[1] = 'u';
                sprintf_s(b + 2, 6, "%04x", (int)ch);
                o += b;
            }
            else o += ch;
        }
    }
    return o;
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}


// 从 JSON 文本里取 "key": "value"
static std::wstring JStr(const std::string& j, const char* key)
{
    std::string k = std::string("\"") + key + "\"";
    auto p = j.find(k);
    if (p == std::string::npos) return {};
    p = j.find(':', p);
    if (p == std::string::npos) return {};
    p = j.find('"', p);
    if (p == std::string::npos) return {};
    auto q = p + 1;
    std::string out;
    while (q < j.size() && j[q] != '"')
    {
        if (j[q] == '\\' && q + 1 < j.size()) { out += j[q + 1]; q += 2; continue; }
        out += j[q++];
    }
    return Utf8ToWide(out);
}

// 取 JSON 对象 {...} 的内部文本（不处理嵌套，够用）
static std::string JObjBody(const std::string& j, const char* key)
{
    std::string k = std::string("\"") + key + "\"";
    auto p = j.find(k);
    if (p == std::string::npos) return {};
    auto lb = j.find('{', p);
    if (lb == std::string::npos) return {};
    return JsonSlice(j, lb, '{', '}');
}

// 取 JSON 数组 [...] 的内部文本
static std::string JArrBody(const std::string& j, const char* key)
{
    std::string k = std::string("\"") + key + "\"";
    auto p = j.find(k);
    if (p == std::string::npos) return {};
    auto lb = j.find('[', p);
    if (lb == std::string::npos) return {};
    return JsonSlice(j, lb, '[', ']');
}

// 从对象体里逐个取 "键": 值
template <typename F>
static void ForEachPair(const std::string& body, F cb)
{
    size_t i = 0;
    while (i < body.size())
    {
        size_t k0 = body.find('"', i);
        if (k0 == std::string::npos) break;
        size_t k1 = JsonSkipString(body, k0);
        if (k1 <= k0 + 1) { i = k1; continue; }
        std::string key = JsonUnescape(body, k0 + 1, k1 - 1);

        size_t colon = body.find(':', k1);
        if (colon == std::string::npos) break;
        size_t v = colon + 1;
        while (v < body.size() && (body[v] == ' ' || body[v] == '\t')) ++v;

        std::string val;
        if (v < body.size() && body[v] == '"')
        {
            size_t e = JsonSkipString(body, v);
            val = JsonUnescape(body, v + 1, e - 1);
            i = e;
        }
        else
        {
            size_t e = body.find_first_of(",}", v);
            if (e == std::string::npos) e = body.size();
            val.assign(body, v, e - v);
            while (!val.empty() && (val.back() == ' ' || val.back() == '\r' || val.back() == '\n')) val.pop_back();
            i = e;
        }
        if (!key.empty()) cb(key, val);
        if (i < body.size() && body[i] == ',') ++i;
    }
}

// 取数值字段的原始文本（用来区分「0.102499 老格式」和「10 新格式」）
static std::string JNumRaw(const std::string& j, const char* key)
{
    std::string k = std::string("\"") + key + "\"";
    auto p = j.find(k);
    if (p == std::string::npos) return {};
    p = j.find(':', p);
    if (p == std::string::npos) return {};
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '	')) ++p;
    size_t e = p;
    while (e < j.size() && ((j[e] >= '0' && j[e] <= '9') || j[e] == '.' || j[e] == '-' || j[e] == '+')) ++e;
    return j.substr(p, e - p);
}

static double JNum(const std::string& j, const char* key, double def)
{
    std::string k = std::string("\"") + key + "\"";
    auto p = j.find(k);
    if (p == std::string::npos) return def;
    p = j.find(':', p);
    if (p == std::string::npos) return def;
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '	')) ++p;
    // 只有真的以数字开头才解析。否则 atof("true") / atof("null") 会返回 0，
    // 把「音量 1.0」静默变成「音量 0」——用户会以为程序坏了、完全没声音。
    if (p >= j.size()) return def;
    char c = j[p];
    if (!(c == '-' || c == '+' || (c >= '0' && c <= '9'))) return def;
    return atof(j.c_str() + p);
}

static bool JBool(const std::string& j, const char* key, bool def)
{
    std::string k = std::string(1, (char)0x22) + key + std::string(1, (char)0x22);
    auto p = j.find(k);
    if (p == std::string::npos) return def;
    p = j.find(':', p);
    if (p == std::string::npos) return def;
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '	')) ++p;
    if (j.compare(p, 4, "true") == 0) return true;
    if (j.compare(p, 5, "false") == 0) return false;
    return def;
}

static std::vector<std::wstring> JStrArray(const std::string& j, const char* key)
{
    std::vector<std::wstring> out;
    std::string k = std::string("\"") + key + "\"";
    auto p = j.find(k);
    if (p == std::string::npos) return out;
    p = j.find('[', p);
    if (p == std::string::npos) return out;
    auto e = j.find(']', p);
    if (e == std::string::npos) return out;
    std::string body = j.substr(p + 1, e - p - 1);
    size_t i = 0;
    while ((i = body.find('"', i)) != std::string::npos)
    {
        std::string cur;
        size_t q = i + 1;
        while (q < body.size() && body[q] != '"')
        {
            if (body[q] == '\\' && q + 1 < body.size()) { cur += body[q + 1]; q += 2; continue; }
            cur += body[q++];
        }
        out.push_back(Utf8ToWide(cur));
        i = q + 1;
    }
    return out;
}

static void LoadSettings()
{
    EnsureSettingsPath();
    std::string j = ReadAll(::g.settingsPath);

    // 主文件读不出来（不存在 / 空 / 被截断 / 磁盘故障）就用上次的备份。
    // 以前这里直接 return，结果是「设置全丢」但界面不报错，用户以为程序坏了。
    if (!LooksLikeJson(j))
    {
        std::string bak = ReadAll(::g.settingsPath + L".bak");
        std::string picked = PickSettingsText(j, bak);
        if (!picked.empty())
        {
            PerfLog("SETTINGS 主文件%s，已改用 .bak 恢复", j.empty() ? "不存在或为空" : "不完整");
            j = picked;
        }
    }
    if (!LooksLikeJson(j))
    {
        // 首次运行也会走到这里（两个文件都不存在），不必报错刷日志。
        if (!j.empty()) PerfLog("SETTINGS 内容无法解析，按默认值启动");
        return;
    }

    // 默认值 = 当前这台机器上的调好状态（浅色主题）
    auto theme = JStr(j, "Theme");
    if (!theme.empty()) { ::g.themeLight = (theme == L"light"); ::g.th = ::g.themeLight ? LightTheme() : DarkTheme(); }
    ::g.fixedMode = (JStr(j, "OutputMode") == L"fixed");   // 缺省时下面 fixedMode 初始值生效
    ::g.routeTarget = JStr(j, "RouteTarget");
    ::g.routePrev = JStr(j, "RoutePrev");
    ::g.routeDirty = JBool(j, "RouteDirty", false);
    {
        // 旧格式是 0..1 的小数（0.102499），新格式是整数百分比（10）。
        // 靠「有没有小数点」区分，两种都能读。
        std::string raw = JNumRaw(j, "PhoneVolume");
        double v = JNum(j, "PhoneVolume", 50.0);
        if (raw.find('.') != std::string::npos) v *= 100.0;    // 老数据：分数 → 百分比
        ::g.phoneVol = App::QuantizeVol((float)(v / 100.0));
    }
    ::g.phoneMute = JBool(j, "PhoneMuted", false);
    ::g.lastDeviceId = JStr(j, "LastDeviceId");
    ::g.lastDeviceName = JStr(j, "LastDeviceName");
    {
        // 恢复上次 arm 过的无线 IP，拔线/重启后仍能自动升级到 ADB
        auto wip = WideToUtf8(JStr(j, "WirelessIp"));
        if (!wip.empty()) ::g.adb.SetWirelessIp(wip);
    }
    {
        // 逐设备音量 / 静音 / 模式
        ::g.volMode = ((int)JNum(j, "VolMode", 1) == 1) ? 1 : 0;
        ::g.devVolPref.clear();
        ::g.devMutePref.clear();

        ForEachPair(JObjBody(j, "DevVol"), [](const std::string& k, const std::string& v) {
            if (k.empty() || v.empty()) return;
            {
                double x = atof(v.c_str());
                if (v.find('.') != std::string::npos) x *= 100.0;   // 老数据：分数 → 百分比
                // 手改 / 旧版本 / 读到半截数字都可能出现超界值，钳住再存，
                // 否则 QuantizeVol 会把 0.0 当合法值悄悄吃掉（音量变 0，用户以为坏了）。
                ::g.devVolPref[Utf8ToWide(k)] = App::QuantizeVol((float)(ClampPercent(x) / 100.0));
            }
        });
        ForEachPair(JObjBody(j, "DevMute"), [](const std::string& k, const std::string& v) {
            if (k.empty()) return;
            ::g.devMutePref[Utf8ToWide(k)] = (v == "true");
        });
        // 清掉永远不可能再匹配上的旧键：随机端口 / mDNS 名字形式。
        // 这些是以前用 serial 当键留下的，同一台设备会占好几条。
        {
            // 新格式的键是 adb:<ro.serialno>，里面不可能有冒号。
            // 带冒号的就是旧的「按 serial 记」的键（ip:5555 / ip:随机端口 / mDNS 名字），
            // 这些 serial 换条链路就变，留着永远匹配不上。
            auto stale = [](const std::wstring& k) {
                if (k.rfind(L"adb:", 0) != 0) return false;
                std::wstring v = k.substr(4);
                return v.find(L':') != std::wstring::npos;
            };
            size_t before = ::g.devVolPref.size() + ::g.devMutePref.size();
            for (auto it = ::g.devVolPref.begin(); it != ::g.devVolPref.end();)
                it = stale(it->first) ? ::g.devVolPref.erase(it) : std::next(it);
            for (auto it = ::g.devMutePref.begin(); it != ::g.devMutePref.end();)
                it = stale(it->first) ? ::g.devMutePref.erase(it) : std::next(it);
            size_t after = ::g.devVolPref.size() + ::g.devMutePref.size();
            if (after != before) PerfLog("SETTINGS 清理 DevVol/DevMute 旧键：%zu → %zu", before, after);
        }

        ForEachPair(JObjBody(j, "BtHistory"), [](const std::string& k, const std::string& v) {
            if (k.empty()) return;
            ::g.btHistory[k] = Utf8ToWide(v);
        });
        for (auto& v : JsonParseStringArray(JArrBody(j, "BtDeleted")))
            if (!v.empty()) ::g.btDeleted.insert(v);
        ForEachPair(JObjBody(j, "BtMacs"), [](const std::string& k, const std::string& v) {
            if (k.empty() || v.empty()) return;
            ::g.btMacs[Utf8ToWide(k)] = Utf8ToWide(v);
        });
    }
    {
        // 恢复 identity → 蓝牙名 映射（重启后 ADB 设备仍显示蓝牙名）
        auto p = j.find("\"BtNames\"");
        if (p != std::string::npos)
        {
            auto lb = j.find('{', p);
            auto rb = (lb == std::string::npos) ? std::string::npos : j.find('}', lb);
            if (lb != std::string::npos && rb != std::string::npos)
            {
                size_t q = lb + 1;
                while (q < rb)
                {
                    auto k1 = j.find('"', q);
                    if (k1 == std::string::npos || k1 >= rb) break;
                    std::string key;
                    size_t r = k1 + 1;
                    while (r < rb && j[r] != '"') { if (j[r] == '\\' && r + 1 < rb) { key += j[r + 1]; r += 2; continue; } key += j[r++]; }

                    auto k2 = j.find('"', r + 1);
                    if (k2 == std::string::npos || k2 >= rb) break;
                    std::string val;
                    size_t v = k2 + 1;
                    while (v < rb && j[v] != '"') { if (j[v] == '\\' && v + 1 < rb) { val += j[v + 1]; v += 2; continue; } val += j[v++]; }

                    if (!key.empty() && !val.empty())
                        ::g.btNames[Utf8ToWide(key)] = Utf8ToWide(val);
                    q = v + 1;
                }
            }
        }
    }
    ::g.autoStart = JBool(j, "AutoStart", false);
    ::g.autoConnect = JBool(j, "AutoConnect", true);
    ::g.adb.SetLanScan(JBool(j, "LanScan", false));
    ::g.adb.SetAutoAdapt(JBool(j, "AutoAdapt", false));
    ::g.idleReset = JBool(j, "IdleReset", true);
    ::g.autoUpgrade = JBool(j, "AutoUpgrade", true);
    ::g.autoReconnect = JBool(j, "AutoReconnect", true);
    ::g.adb.SetAutoReconnect(::g.autoReconnect);
    ::g.adb.SetDualChannel(JBool(j, "DualChannel", false));
    ::g.adb.SetAutoDual(JBool(j, "AutoDual", false));
    ::g.setupDone = JBool(j, "SetupDone", false);
    ::g.silentStart = JBool(j, "SilentStart", true);
    ::g.adb.SetBufferGear(std::clamp((int)JNum(j, "BufGear", 2), 0, 3));   // 默认「标准」
    {
        // 恢复「希望保持转发」的设备，启动后自动开始转发（不依赖蓝牙触发）
        std::set<std::string> wd;
        for (auto& v : JsonParseStringArray(JArrBody(j, "WantedDevices")))
            if (!v.empty()) wd.insert(v);
        // 注意：不要把 wd 塞进 adb 的 wanted_，
        // 否则没勾选的设备也会被断线自愈自动拉起。勾选框（autoDevices）是唯一依据。
        // 老版本没有 AutoDevices 时，把 WantedDevices 迁过来当勾选项
        bool hasAutoKey = (j.find("\"AutoDevices\"") != std::string::npos);
        for (auto& v : JsonParseStringArray(JArrBody(j, "AutoDevices")))
            if (!v.empty()) ::g.autoDevices.insert(v);
        // 只有「老配置里根本没有 AutoDevices 这个键」才从 WantedDevices 迁移。
        // 以前判 autoDevices.empty()：用户把所有设备都取消勾选后重启，
        // 勾选状态会被 WantedDevices 整体还原 —— 取消了等于没取消。
        if (!hasAutoKey) { for (auto& v : wd) ::g.autoDevices.insert(v); }
        // 有「希望转发」的设备 → 启动即进入 ADB 模式，界面和实际状态保持一致
        // 只要配置过「自动连接」的设备，启动就直接进 ADB 模式。
        // 注意不能只看 WantedDevices —— 它要等设备真正开始转发才有值，会形成循环依赖。
        if (!wd.empty() || !::g.autoDevices.empty()) ::g.useAdb = true;
    }
    {
        // 恢复已知无线设备列表（用于自动重连）
        std::set<std::string> kw;
        for (auto& v : JsonParseStringArray(JArrBody(j, "KnownWireless")))
            if (!v.empty()) kw.insert(v);
        // 只剔除 mDNS 名字（不能 adb connect），端口形式全部保留
        {
            std::set<std::string> clean;
            for (auto& v : kw)
                if (v.find("_adb-tls-") == std::string::npos) clean.insert(v);
            kw.swap(clean);
        }
        ::g.adb.SetKnownWireless(kw);
    }
    {
        std::map<std::string, int> bp;
        ForEachPair(JObjBody(j, "BufPref"), [&bp](const std::string& k, const std::string& v) {
            // BufPref 是「每台设备学到的缓冲毫秒数」，不是挡位！
            // （挡位是另一个字段 BufGear，见上面 1232 行，那里才是 0..3）
            // 这里只防越界/脏值：过小的缓冲会导致持续欠载，过大则延迟失控。
            if (k.empty() || v.empty()) return;
            bp[k] = ClampBufferMs(atoi(v.c_str()));
        });
        ::g.adb.SetBufferPrefs(bp);
    }
    auto ca = JStr(j, "CloseAction"); if (!ca.empty()) ::g.closeAction = ca;
    auto ma = JStr(j, "MinimizeAction"); if (!ma.empty()) ::g.minAction = ma;

    auto prio = JStrArray(j, "OutputPriority");
    if (!prio.empty()) ::g.prio = prio;
}

#include "settings_save.inc"

static void Paint(HWND hwnd);
static void RenderTo(HDC target, int w, int h);
static bool LinkUp();
static const OutDevice* FindOut(const std::wstring& id);
static void RefreshCurrentDefault();
static void SavePrio();

// ------------------------------------------------------------------ 布局 + 绘制

struct Notice
{
    bool show = false;
    int level = 0;
    std::wstring text;
    int actionId = 0;
    std::wstring action;
};

static Notice ComputeNotice();

struct Layout
{
    RECTF titleBar, footer, leftCol, rightCol;
    RECTF devCard, statusCard, phoneCard, outCard;
    RECTF devList, prioList, notice;
};

static float S(float v) { return v * ::g.dpi; }

// 系统版本：AudioPlaybackConnection 需要 Win10 1809（build 17763）及以上
static bool IsWindowsVersionOk(std::wstring* detail = nullptr)
{
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);

    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto fn = nt ? (RtlGetVersionFn)GetProcAddress(nt, "RtlGetVersion") : nullptr;
    if (!fn || fn(&vi) != 0)
    {
        if (detail) *detail = L"无法读取系统版本";
        return true;   // 读不到就不拦，避免误伤
    }
    if (detail)
        *detail = L"Windows " + std::to_wstring(vi.dwMajorVersion) + L"."
                + std::to_wstring(vi.dwMinorVersion) + L" (build "
                + std::to_wstring(vi.dwBuildNumber) + L")";

    if (vi.dwMajorVersion > 10) return true;
    if (vi.dwMajorVersion < 10) return false;
    return vi.dwBuildNumber >= 17763;    // Win10 1809
}

// 顶部提示条内容：按严重程度取「当前最该告诉用户的那一件事」
static Notice ComputeNotice()
{
    Notice n;

    // ⓪ 系统版本过低：任何功能都不会正常，优先级最高
    {
        static bool verOk = IsWindowsVersionOk();
        if (!verOk)
        {
            n.show = true; n.level = 2;
            n.text = L"系统版本过低：需要 Windows 10 1809 或更高";
            return n;
        }
    }

    // ① 随包组件缺失：ADB 通道不可用，但蓝牙链路完全正常（这正是「只发 exe」的交付形态）。
    // 所以不能一律报红：红条等于告诉用户「这软件坏了」，而它其实好好的。
    if (!::g.adb.HasAdb() || !::g.adb.HasScrcpy() || !::g.adb.HasServer())
    {
        // 单文件版释放失败时，「缺少 adb.exe」会误导人去找 tools 文件夹，要说真实原因
        if (!::g.adb.EmbedError().empty())
        {
            n.show = true; n.level = 2;
            n.text = L"运行时文件释放失败：" + ::g.adb.EmbedError() + L"。可先清空临时文件夹再试。";
        }
        else if (::g.useAdb)
        {
            // 用户当前就在 ADB 页，这时确实用不了，要说清怎么恢复
            n.show = true; n.level = 2;
            n.text = L"ADB 通道不可用：缺少 tools 文件夹。只用蓝牙可以不管它。";
        }
        else
        {
            n.show = true; n.level = 1;
            n.text = L"蓝牙可以正常使用。想用低延迟的 ADB 通道，需要把 tools 文件夹放到程序旁边。";
        }
        n.actionId = ID_CHECK; n.action = L"自检";
        return n;
    }


    // ① 有设备未授权（最常见、也最需要用户动手）
    for (auto& d : ::g.adb.UiDevices())
    {
        if (d.state == "unauthorized")
        {
            n.show = true; n.level = 2;
            n.text = L"设备未授权：请在设备屏幕上点「允许 USB 调试」";
            n.actionId = ID_CHECK; n.action = L"自检";
            return n;
        }
    }

    // ② 瞬时提示（刚才那次操作的结果）：优先级高于下面的常驻判断，
    // 否则「升级失败：已试过…」会被「未找到设备」盖掉
    {
        std::wstring nt = ::g.GetNoticeText();
        if (!nt.empty() && GetTickCount64() < ::g.GetNoticeUntil())
        {
            n.show = true; n.level = ::g.GetNoticeLevel();
            n.text = nt;
            return n;
        }
    }

    // ③ 配了自动连接却一台都没找到
    if (::g.useAdb && !::g.autoDevices.empty() && ::g.adb.UiDevices().empty())
    {
        n.show = true; n.level = 2;
        if (::g.adb.UiWirelessAllFailed())
        {
            // 所有记得的 IP 都拒绝连接 = 手机上的 ADB 端口没开
            n.text = L"手机上的无线调试已关闭（端口拒绝连接）。插一次 USB 线即可自动恢复；"
                     L"Android 11+ 也可以用设置里的「无线调试配对」一劳永逸。";
            n.actionId = ID_SETTINGS; n.action = L"去设置";
        }
        else
        {
            n.text = L"未找到设备：请插一次 USB 线，或确认设备已开启无线调试";
            n.actionId = ID_HELP; n.action = L"怎么开？";
        }
        return n;
    }

    // ④ 动作结果里带「失败 / 未 / 错误」的，也要显眼
    std::wstring hintNow = ::g.GetAdbHint();
    if (::g.useAdb && !hintNow.empty())
    {
        const std::wstring& h = hintNow;
        if (h.find(L"失败") != std::wstring::npos ||
            h.find(L"未") != std::wstring::npos ||
            h.find(L"错误") != std::wstring::npos)
        {
            n.show = true; n.level = 1;
            n.text = h;
            return n;
        }
    }

    return n;
}

// 单行 + 超长省略号：标题栏空间有限，长文案必须截断而不是压住别的元素
static void TextEllipsis(Graphics& g, const std::wstring& s, float x, float y, float w, float px, COLORREF c)
{
    if (w <= S(8)) return;
    Font& f = GetFont(px, false);
    SolidBrush br(C(c));
    StringFormat sf;
    sf.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(s.c_str(), (INT)s.size(), &f, RectF(x, y, w, px * 1.8f), &sf, &br);
}

// 最多两行 + 省略号：提示条里有些关键信息一行放不下，
// 被截掉的那半句恰恰是用户最需要看到的。
// 两行的高度必须按「实际行高」给，不能按 px 估：
// 11.5px 字体行高约 15.3px，两行要 31px；给 px*2.2=25px 时第二行放不下，
// LineLimit 会把整段裁成一行加省略号（实测就是这样被截断的）。
static void TextEllipsis2(Graphics& g, const std::wstring& s, float x, float y, float w, float px, COLORREF c)
{
    if (w <= S(8)) return;
    Font& f = GetFont(px, false);
    SolidBrush br(C(c));
    StringFormat sf;
    sf.SetFormatFlags(StringFormatFlagsLineLimit);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(s.c_str(), (INT)s.size(), &f, RectF(x, y, w, px * 3.0f), &sf, &br);
}

static Layout ComputeLayout(int w, int h)
{
    Layout L{};
    float pad = S(24), gap = S(26), cardGap = S(10), footerH = S(52), titleH = S(46);

    L.titleBar = RectF(0, 0, (float)w, titleH);
    L.footer = RectF(0, h - footerH, (float)w, footerH);

    // 提示条改为内联在标题栏空白区，不再占正文高度。
    // 原来有提示时 bodyTop 会下移 42px，导致切页时整页跳一下 —— 这是「位置会变」的根因。
    float bodyTop = titleH + S(6);
    L.notice = RectF(0, 0, 0, 0);   // 由 DrawTitleBar 内联绘制

    float bodyH = h - footerH - bodyTop - S(10);
    float availW = w - pad * 2 - gap;
    float leftW = availW * (1.0f / 2.25f);
    float rightW = availW - leftW;

    L.leftCol = RectF(pad, bodyTop, leftW, bodyH);
    L.rightCol = RectF(pad + leftW + gap, bodyTop, rightW, bodyH);

    // 左：设备卡占满整栏（状态信息已分散到标题栏/右栏/列表标签）
    L.devCard = RectF(L.leftCol.X, L.leftCol.Y, L.leftCol.Width, bodyH);

    // 右：手机声音卡 + 输出设备卡
    float phoneH = S(116);
    L.phoneCard = RectF(L.rightCol.X, L.rightCol.Y, L.rightCol.Width, phoneH);
    L.outCard = RectF(L.rightCol.X, L.phoneCard.GetBottom() + cardGap, L.rightCol.Width,
                      bodyH - phoneH - cardGap);

    return L;
}

// 顶部提示条：有色底 + 左侧色条 + 可选动作按钮
static void DrawTitleBar(Graphics& g, const Layout& L)
{
    // 图标：白底深闪电（深色）/ 深底白闪电（浅色）
    float ix = S(18), iy = S(14), is = S(16);
    COLORREF chip = ::g.themeLight ? Hex(L"#1A1D21") : Hex(L"#EDEFF2");
    COLORREF bolt = ::g.themeLight ? Hex(L"#F7F8FA") : Hex(L"#14171A");
    FillRound(g, RectF(ix, iy, is, is), S(5), chip);
    GraphicsPath bp;
    PointF pts[6] = {
        PointF(ix + is * 0.58f, iy + is * 0.12f), PointF(ix + is * 0.30f, iy + is * 0.56f),
        PointF(ix + is * 0.47f, iy + is * 0.56f), PointF(ix + is * 0.38f, iy + is * 0.88f),
        PointF(ix + is * 0.70f, iy + is * 0.41f), PointF(ix + is * 0.52f, iy + is * 0.41f)
    };
    bp.AddPolygon(pts, 6);
    SolidBrush bb(C(bolt));
    g.FillPath(&bb, &bp);

    Text(g, L"WaveTether", ix + is + S(9), S(12), S(13.5f), ::g.th.Text, true);

    // 状态点 + 文案（与音量卡同一套口径）
    bool linkUp = LinkUp();
    auto st = ::g.MakeStatus();
    COLORREF dot = st.color;

    if (!linkUp)
    {
        auto bs = ::g.bt.State();
        if (bs == LinkState::Connecting)      { st.text = L"连接中…";   st.color = ::g.th.Warn; }
        else if (bs == LinkState::Listening)  { st.text = L"等待连接…"; st.color = ::g.th.Warn; }
        else if (bs == LinkState::Failed)     { st.text = L"连接失败";  st.color = ::g.th.Err; }
        else                                  { st.text = L"未连接";    st.color = ::g.th.TextFaint; }
        dot = st.color;
    }

    // 标题栏自己画状态点，文案里去掉重复的 "● "
    std::wstring titleSt = st.text;
    if (titleSt.rfind(L"● ", 0) == 0) titleSt.erase(0, 2);
    size_t mid = titleSt.find(L" · ● ");
    if (mid != std::wstring::npos) titleSt.erase(mid + 3, 2);

    float dx = ix + is + S(9) + TextWidth(g, L"WaveTether", S(13.5f), true) + S(18);
    SolidBrush db(C(dot));
    g.FillEllipse(&db, dx, S(19), S(6), S(6));
    Text(g, titleSt, dx + S(13), S(14), S(12), st.color);

    // ---- 提示条：内联在「状态文字」与「右上按钮」之间的空白区 ----
    // 好处：不占正文高度，切页时布局不会跳；代价：只有一行，超长要省略号
    {
        Notice n = ComputeNotice();
        float bwT = S(40), bhT = S(28);
        RECT client2; GetClientRect(g_hwnd, &client2);
        float rightX = client2.right - S(10) - bwT * 3 - S(4);
        float x0 = dx + S(13) + TextWidth(g, titleSt, S(12)) + S(20);
        float avail = rightX - S(14) - x0;

        ::g.noticeBtnRect = RECTF(0, 0, 0, 0);
        if (n.show && avail > S(90))
        {
            COLORREF accent = (n.level == 2) ? ::g.th.Err
                            : (n.level == 1) ? ::g.th.Warn
                                             : ::g.th.AccentText;
            bool hasAction = (n.actionId != 0 && !n.action.empty());
            float btnW = S(58), btnH = S(22);
            float textW = avail - (hasAction ? btnW + S(18) : 0);
            float ty = S(14);

            // 左侧短色条代替原来整条横幅的视觉重量
            FillRound(g, RECTF(x0, ty + S(1), S(2.5f), S(14)), S(1.2f), accent);
            TextEllipsis2(g, n.text, x0 + S(9), ty - S(2), textW - S(9), S(11.5f), ::g.th.Text);

            if (hasAction)
            {
                RECTF btn(x0 + avail - btnW, ty - S(4), btnW, btnH);
                if (::g.hover == n.actionId) FillRound(g, btn, S(5), ::g.th.HoverFill);
                else StrokeRound(g, btn, S(5), ::g.th.ControlBorder, 1);
                TextCenter(g, n.action, btn, S(11.f), accent, true);
                ::g.hits.push_back({ n.actionId, -1, btn });
                ::g.noticeBtnRect = btn;   // NCHITTEST 要放行，否则真鼠标点它只会拖动窗口
            }
        }
    }

    // 右上三个按钮
    float bw = S(40), bh = S(28), bx = (float)GetSystemMetrics(SM_CXSCREEN);
    RECT client; GetClientRect(g_hwnd, &client);
    bx = client.right - S(10) - bw * 3 - S(4);
    float by = S(8);

    // 主题
    RECTF rTheme(bx, by, bw, bh);
    if (::g.hover == ID_TITLE_THEME) FillRound(g, rTheme, S(6), ::g.th.HoverFill);
    float ccx = bx + bw / 2, ccy = by + bh / 2, rr = S(6.2f);
    {
        Pen ring(C(::g.th.TextMuted), S(1.1f));
        g.DrawEllipse(&ring, ccx - rr, ccy - rr, rr * 2, rr * 2);
        GraphicsPath half;
        half.AddArc(ccx - rr + S(0.8f), ccy - rr + S(0.8f), (rr - S(0.8f)) * 2, (rr - S(0.8f)) * 2, 90, 180);
        half.CloseFigure();
        SolidBrush fb(C(::g.th.TextMuted));
        g.FillPath(&fb, &half);
    }

    // 最小化
    RECTF rMin(bx + bw + S(4), by, bw, bh);
    if (::g.hover == ID_TITLE_MIN) FillRound(g, rMin, S(6), ::g.th.HoverFill);
    {
        Pen p(C(::g.th.Text), S(1.2f));
        p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
        g.DrawLine(&p, rMin.X + bw / 2 - S(5), rMin.Y + bh / 2, rMin.X + bw / 2 + S(5), rMin.Y + bh / 2);
    }

    // 关闭
    RECTF rClose(bx + (bw + S(4)) * 2, by, bw, bh);
    if (::g.hover == ID_TITLE_CLOSE) FillRound(g, rClose, S(6), ::g.th.HoverFill);
    {
        Pen p(C(::g.th.Text), S(1.2f));
        p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
        float hx = rClose.X + bw / 2, hy = rClose.Y + bh / 2, e = S(4.2f);
        g.DrawLine(&p, hx - e, hy - e, hx + e, hy + e);
        g.DrawLine(&p, hx + e, hy - e, hx - e, hy + e);
    }

    ::g.hits.push_back({ ID_TITLE_THEME, -1, rTheme });
    ::g.hits.push_back({ ID_TITLE_MIN, -1, rMin });
    ::g.hits.push_back({ ID_TITLE_CLOSE, -1, rClose });
}

static void DrawCheck(Graphics& g, float x, float y, bool on, float size)
{
    RECTF box(x, y, size, size);
    if (on)
    {
        FillRound(g, box, S(4), ::g.th.AccentText);
        // 喇叭图形（与 WPF 版一致）
        float k = size / 15.f;
        GraphicsPath body;
        PointF pts[6] = {
            PointF(x + 3.1f * k, y + 6.0f * k), PointF(x + 5.0f * k, y + 6.0f * k),
            PointF(x + 8.0f * k, y + 3.7f * k), PointF(x + 8.0f * k, y + 11.3f * k),
            PointF(x + 5.0f * k, y + 9.0f * k), PointF(x + 3.1f * k, y + 9.0f * k)
        };
        body.AddPolygon(pts, 6);
        SolidBrush b(C(::g.th.OnAccent));
        g.FillPath(&b, &body);
        Pen arc(C(::g.th.OnAccent), S(1.35f) * k * 1.1f);
        arc.SetStartCap(LineCapRound); arc.SetEndCap(LineCapRound);
        g.DrawArc(&arc, x + 6.2f * k, y + 4.8f * k, 6.8f * k, 5.4f * k, -60, 120);
    }
    else
    {
        FillRound(g, box, S(4), ::g.th.ControlBg);
        StrokeRound(g, box, S(4), ::g.th.ControlBorder, S(1.2f));
    }
}

static void DrawRadio(Graphics& g, float x, float y, bool on)
{
    float d = S(15);
    RECTF box(x, y, d, d);
    SolidBrush bg(C(::g.th.ControlBg));
    g.FillEllipse(&bg, box.X, box.Y, d, d);
    Pen ring(C(on ? ::g.th.AccentText : ::g.th.ControlBorder), S(1.2f));
    g.DrawEllipse(&ring, box.X, box.Y, d, d);
    if (on)
    {
        SolidBrush dot(C(::g.th.AccentText));
        g.FillEllipse(&dot, box.X + S(4), box.Y + S(4), d - S(8), d - S(8));
    }
}

// 设置面板（覆盖层，不另开窗口）

static const wchar_t* HelpTitle(int topic)
{
    switch (topic)
    {
    case HELP_CHANNEL: return L"蓝牙 / ADB 通道";
    case HELP_PAIR:    return L"配对与升级";
    case HELP_VOLUME:  return L"音量是怎么工作的";
    case HELP_OUTPUT:  return L"输出设备优先级";
    case HELP_FIXED:   return L"连接时切换输出";
    case HELP_MULTI:   return L"多设备同时播放";
    case HELP_STATUS:  return L"状态与图标含义";
    case HELP_TROUBLE: return L"常见问题";
    case HELP_WHY:     return L"为什么这样设计";
    case HELP_RUNTIME: return L"运行时文件";
    default:           return L"帮助";
    }
}

#include "help_body.inc"

// 自检：用当前状态给出可操作的结论（全部读缓存，不做 adb IO）
static float DrawSelfCheck(Graphics& g, float x, float y, float w)
{
    struct Item { int level; std::wstring title; std::wstring advice; };  // 0 好 / 1 注意 / 2 问题
    std::vector<Item> items;

    // 第一项先说「你手上是哪个版本」。用户报问题时这一条能直接排除一大类误会：
    // 单文件版会释放运行时、便携版用旁边的 tools、蓝牙版根本没有 ADB。
    {
        bool embedded = !::g.adb.EmbeddedRoot().empty();
        if (embedded)
            items.push_back({ 0, L"版本形态：单文件版",
                              L"内嵌工具已释放到临时目录，当前用的是那份拷贝。" });
        else if (::g.adb.HasAdb())
            items.push_back({ 0, L"版本形态：便携版",
                              L"用的是 exe 旁边的 tools 文件夹。" });
        else
            items.push_back({ 1, L"版本形态：蓝牙版",
                              L"没有 ADB 组件，只能走蓝牙；想用 ADB 就换单文件版或带 tools 的便携版。" });
    }

    auto devs = ::g.adb.UiDevices();
    bool anyDev = !devs.empty();
    bool anyUnauth = false, anyAuth = false;
    for (auto& d : devs)
    {
        if (d.state == "unauthorized") anyUnauth = true;
        if (d.state == "device") anyAuth = true;
    }
    int running = ::g.adb.UiRunningCount();

    // 首项就说「组件齐不齐」：它决定了下面所有 ADB 项是否有意义，
    // 而且「只发 exe」的用户一打开自检就能看到「这不是故障」
    {
        bool ok = ::g.adb.HasAdb() && ::g.adb.HasScrcpy() && ::g.adb.HasServer();
        if (!::g.adb.EmbedError().empty())
            items.push_back({ 2, L"运行时文件释放失败", ::g.adb.EmbedError() + L"。可先清空临时文件夹再试。" });
        else if (ok)
            items.push_back({ 0, L"程序组件完整", L"" });
        else
            items.push_back({ 1, L"未安装 ADB 组件（只影响 ADB，不影响蓝牙）",
                L"把 tools 文件夹放到程序旁边就能用低延迟的 ADB 通道；不用 ADB 的话可以忽略。" });
    }

    // 蓝牙链路单独一项：自检面板原来全是 ADB 项，只用蓝牙的人打开它看不到任何相关信息
    {
        LinkState btState = ::g.bt.State();
        // 不能用 btDevs 判断：它只在进过蓝牙页、枚举完成后才有值。
        // 用户停在 ADB 页打开自检时会误报「还没有配对任何蓝牙设备」。
        // 历史记录（BtHistory）和 identity 映射（btNames）是持久化的，能反映真实情况。
        bool btPaired = !::g.btDevs.empty() || !::g.btHistory.empty() || !::g.btNames.empty();
        if (btState == LinkState::Connected)
            items.push_back({ 0, L"蓝牙已连接" + (::g.bt.DeviceName().empty() ? std::wstring()
                                                          : (L"：" + ::g.bt.DeviceName())),
                              L"蓝牙只在有声音时才建立音频链路，没声音时显示「未连接」是正常的。" });
        else if (btState == LinkState::Connecting)
            items.push_back({ 1, L"蓝牙正在连接…", L"如果一直停在这里，检查手机的蓝牙是否打开、是否在范围内。" });
        else if (btPaired)
            items.push_back({ 0, L"蓝牙已配对，当前未连接",
                              L"正常现象：没声音时蓝牙会自己断开。想用蓝牙就点「蓝牙」页下方的「连接」。" });
        else
            items.push_back({ 1, L"还没有配对任何蓝牙设备",
                              L"点「蓝牙」页列表上方那一行可以打开 Windows 蓝牙设置去配对。" });
    }

    // 下面三项只对「能用 ADB」的版本有意义。蓝牙版里它们永远是「没设备/没转发」，
    // 会被误判成红项，让只用蓝牙的人一打开自检就被吓一跳。
    if (::g.adb.HasAdb())
    {
    items.push_back({ anyDev ? 0 : 2,
        anyDev ? L"已发现设备" : L"没有发现任何设备",
        anyDev ? (L"共 " + std::to_wstring(devs.size()) + L" 台")
               : L"用 USB 线连一次电脑并允许调试，之后会自动转成无线。设备重启过就要再插一次线。" });

    items.push_back({ anyUnauth ? 2 : (anyAuth ? 0 : 1),
        anyUnauth ? L"设备未授权" : (anyAuth ? L"设备已授权" : L"等待授权"),
        anyUnauth ? L"请看设备屏幕，点「允许 USB 调试」（建议勾选「始终允许」）。"
                  : (anyAuth ? L"" : L"插线并确认设备上的授权弹窗。") });

    items.push_back({ running > 0 ? 0 : (anyAuth ? 1 : 2),
        running > 0 ? (L"正在转发 " + std::to_wstring(running) + L" 台设备") : L"没有在转发",
        running > 0 ? L"" : L"在设备列表里勾选设备，或点右侧 ▶ 手动开始。" });
    }

    bool anyPlaying = false;
    for (auto& e : ::g.devVols) if (e.playing) anyPlaying = true;
    items.push_back({ anyPlaying ? 0 : 1,
        anyPlaying ? L"电脑正在接收音频" : L"电脑端没有声音",
        anyPlaying ? std::wstring()
                   : std::wstring(L"若设备确实在播放，按顺序查：\n"
                                  L"① 设备自身音量是否为 0\n"
                                  L"② 蓝牙是否还连着电脑（会抢走音频，程序会自动断开）\n"
                                  L"③ 点设备右侧 ■ 再 ▶，重启这一路的转发") });

    // 蓝牙占用 / 缓冲
    if (running > 0)
    {
        std::wstring buf;
        for (auto& ser : ::g.adb.UiRunningSerials())
        {
            if (!buf.empty()) buf += L" · ";
            buf += std::to_wstring(::g.adb.UiBufferMs(ser, 200)) + L"ms";
            int dd = ::g.adb.UiDrops(ser);
            if (dd > 0) buf += L"（丢 " + std::to_wstring(dd) + L"）";
        }
        items.push_back({ 0, L"当前缓冲", buf + L"。卡顿会自动加大；一直卡就换「保守」挡位。" });
    }

    float py = y;

    // 先给一句结论：不想逐条看的人读完这句就知道要不要慌
    {
        int worst = 0, warn = 0, bad = 0;
        for (auto& it : items)
        {
            worst = std::max(worst, it.level);
            if (it.level == 1) ++warn;
            if (it.level == 2) ++bad;
        }
        std::wstring head;
        COLORREF hc;
        if (worst == 0)
        {
            head = L"结论：全部正常，可以放心用。";
            hc = ::g.th.Ok;
        }
        else if (worst == 1)
        {
            head = L"结论：能用，但有 " + std::to_wstring(warn) + L" 项标黄的值得看一眼。";
            hc = ::g.th.Warn;
        }
        else
        {
            head = L"结论：有 " + std::to_wstring(bad) + L" 项没就绪，标红的就是原因，照着它下面的做法做。";
            hc = ::g.th.Err;
        }
        TextWrap(g, head, x, py, w, S(40), S(13.f), hc);
        py += S(40);
    }

    for (auto& it : items)
    {
        COLORREF c = (it.level == 0) ? ::g.th.Ok : (it.level == 1) ? ::g.th.Warn : ::g.th.Err;
        SolidBrush b(C(c));
        g.FillEllipse(&b, x, py + S(4), S(8), S(8));

        float tw = w - S(20);
        Text(g, it.title, x + S(18), py, S(13.f), ::g.th.Text, it.level == 0, tw);
        py += S(19);
        if (!it.advice.empty())
        {
            int nLines = 1;
            for (wchar_t c : it.advice) if (c == L'\n') nLines++;
            float ah = S(16) * nLines + S(10);
            TextWrap(g, it.advice, x + S(18), py, tw, ah + S(20), S(11.5f), ::g.th.TextMuted);
            py += ah;
        }
        py += S(10);
    }
    return py - y;
}

// 首次设置向导：把「为什么这么麻烦」讲清楚，并实时显示进度

// 无线调试配对对话框（Android 11+）。
// 这是全程序唯一的文本输入界面：手机屏幕上会显示随机端口和 6 位配对码，必须手输。
static void DrawPairDialog(Graphics& g, int w, int h)
{
    ::g.hits.clear();

    SolidBrush dim(Color(180, 0, 0, 0));
    g.FillRectangle(&dim, 0, 0, w, h);

    float cw = S(560), ch = S(312);
    RECTF card((w - cw) / 2, (h - ch) / 2, cw, ch);
    FillRound(g, card, S(12), ::g.th.ControlBg);
    StrokeRound(g, card, S(12), ::g.th.WindowBorder, 1);

    float px = card.X + S(26), pw = cw - S(52);
    float py = card.Y + S(18);

    Text(g, L"无线调试配对", px, py, S(16), ::g.th.Text, true);
    py += S(26);
    TextWrap(g, L"手机：开发者选项 → 点「无线调试」这一行文字（不是右边开关）→ 打开开关 → "
                L"点「使用配对码配对设备」。屏幕上会显示一组地址和 6 位配对码。",
             px, py, pw, S(46), S(11.5f), ::g.th.TextMuted);
    py += S(48);

    struct Field { const wchar_t* label; std::wstring* val; int id; const wchar_t* hint; };
    Field fields[3] = {
        { L"配对地址",  &::g.pairAddr, ID_PAIR_ADDR, L"IP:端口（配对端口）" },
        { L"配对码",    &::g.pairCode, ID_PAIR_CODE, L"手机屏幕上的 6 位数字" },
        { L"连接端口",  &::g.pairPort, ID_PAIR_PORT, L"可留空，自动查找" },
    };

    for (int i = 0; i < 3; ++i)
    {
        Text(g, fields[i].label, px, py + S(4), S(12), ::g.th.TextMuted);
        RECTF box(px + S(76), py, pw - S(76), S(30));
        bool focused = (::g.pairOpen && ::g.pairFocus == i);
        FillRound(g, box, S(6), ::g.th.SelectFill);
        StrokeRound(g, box, S(6), focused ? ::g.th.Accent : ::g.th.ControlBorder, focused ? 2.f : 1.f);

        const std::wstring& v = *fields[i].val;
        if (v.empty() && !focused)
            Text(g, fields[i].hint, box.X + S(9), box.Y + S(7), S(12), ::g.th.TextFaint);
        else
            Text(g, v + (focused ? L"|" : L""), box.X + S(9), box.Y + S(7), S(12.5f), ::g.th.Text);

        ::g.hits.push_back({ fields[i].id, i, box });
        py += S(38);
    }

    // 状态行
    std::wstring st = ::g.pairStatusLocal;
    if (::g.adb.UiPairBusy()) st = L"正在配对…";
    else if (st.empty()) st = ::g.adb.UiPairStatus();
    if (!st.empty())
        TextWrap(g, st, px, py, pw, S(34), S(11.5f), ::g.th.TextMuted);
    py += S(36);

    // 按钮
    float bh = S(32), bwid = S(92);
    RECTF rGo(card.GetRight() - S(26) - bwid, card.GetBottom() - S(18) - bh, bwid, bh);
    RECTF rCancel(rGo.X - S(10) - bwid, rGo.Y, bwid, bh);

    // 按钮三态：配对 → 配对中…（不可点）→ 完成 / 失败后回到配对
    bool busy = ::g.adb.UiPairBusy();
    int  res  = ::g.adb.UiPairResult();
    COLORREF goBg = ::g.th.Accent;
    std::wstring goText = L"配对";
    bool goEnabled = true;
    bool codeEmpty = ::g.pairCode.empty();
    if (busy) { goBg = ::g.th.ControlBorder; goText = L"配对中…"; goEnabled = false; }
    else if (res == 1) { goBg = ::g.th.Ok; goText = L"完成"; }
    else if (res == 2) { goBg = ::g.th.Accent; goText = codeEmpty ? L"重试连接" : L"重试配对"; }
    else if (codeEmpty) { goText = L"连接"; }   // 已配对过的设备不需要配对码

    FillRound(g, rGo, S(7), goBg);
    TextCenter(g, goText, rGo, S(13), busy ? ::g.th.TextMuted : ::g.th.OnAccent, true);
    if (goEnabled) ::g.hits.push_back({ ID_PAIR_GO, -1, rGo });

    FillRound(g, rCancel, S(7), ::g.hover == ID_PAIR_CANCEL ? ::g.th.HoverFill : ::g.th.SelectFill);
    TextCenter(g, L"取消", rCancel, S(13), ::g.th.Text);
    ::g.hits.push_back({ ID_PAIR_CANCEL, -1, rCancel });

    // 手机上刚打开/重开了配对界面时，地址会变，需要能重查
    RECTF rFind(rCancel.X - S(10) - S(96), rCancel.Y, S(96), rCancel.Height);
    FillRound(g, rFind, S(7), ::g.hover == ID_PAIR_FIND ? ::g.th.HoverFill : ::g.th.SelectFill);
    TextCenter(g, L"立即查找", rFind, S(12.5f), ::g.th.TextMuted);
    ::g.hits.push_back({ ID_PAIR_FIND, -1, rFind });
}

static void DrawWizard(Graphics& g, int w, int h)
{
    ::g.hits.clear();

    SolidBrush dim(Color(180, 0, 0, 0));
    g.FillRectangle(&dim, 0, 0, w, h);

    float cw = S(600), ch = S(420);
    RECTF card((w - cw) / 2, (h - ch) / 2, cw, ch);
    FillRound(g, card, S(12), ::g.th.ControlBg);
    StrokeRound(g, card, S(12), ::g.th.WindowBorder, 1);

    float px = card.X + S(28), pw = cw - S(56);
    Text(g, L"首次设置", px, card.Y + S(20), S(16), ::g.th.Text, true);
    Text(g, L"只需要做一次，之后开机自动连",
         px + TextWidth(g, L"首次设置", S(16), true) + S(12), card.Y + S(24), S(11.5f), ::g.th.TextMuted);

    // 蓝牙版（只发了 exe，没有 tools 文件夹）走不到 ADB 三步，必须直说，
    // 否则用户会一直插线、点允许，然后一无所获。
    if (!::g.adb.HasAdb() || !::g.adb.HasScrcpy() || !::g.adb.HasServer())
    {
        TextWrap(g, L"当前是「蓝牙版」（没有 tools 文件夹），ADB 通道不可用。",
                 px, card.Y + S(58), pw, S(24), S(13.f), ::g.th.Warn);
        TextWrap(g, L"直接用蓝牙就行：手机蓝牙里配对这台电脑 → 回到程序点设备行左下角的「连接」。",
                 px, card.Y + S(88), pw, S(40), S(12.5f), ::g.th.Text);
        RECTF rDone0(card.GetRight() - S(28) - S(130), card.GetBottom() - S(50), S(130), S(32));
        FillRound(g, rDone0, S(7), ::g.th.Accent);
        TextCenter(g, L"只用蓝牙，知道了", rDone0, S(13.f), ::g.th.OnAccent, true);
        ::g.hits.push_back({ ID_WIZ_CLOSE, -1, rDone0 });
        return;
    }

    // 实时状态
    auto devs = ::g.adb.UiDevices();
    bool anyAuthorized = false, anyWireless = false, anyUnauth = false;
    for (auto& d : devs)
    {
        if (d.state == "device")
        {
            anyAuthorized = true;
            if (d.serial.find(':') != std::string::npos) anyWireless = true;
        }
        if (d.state == "unauthorized") anyUnauth = true;
    }
    bool adbOk = (::g.adb.UiRunningCount() > 0);

    struct Step { const wchar_t* title; const wchar_t* desc; int state; };  // 0=待做 1=进行中 2=完成
    Step steps[3];
    steps[0] = { L"1. 在设备上开启「USB 调试」",
                 L"设置 → 开发者选项 → 打开「USB 调试」。找不到开发者选项就先连点「版本号」7 次。",
                 (anyAuthorized || anyWireless) ? 2 : 0 };
    steps[1] = { L"2. 插 USB 线，在设备上点「允许」",
                 anyUnauth ? L"检测到设备，请在屏幕上点「允许 USB 调试」"
                           : L"插线后设备会弹出授权提示，勾选「始终允许」再点确定。",
                 anyUnauth ? 1 : (anyAuthorized ? 2 : 0) };
    steps[2] = { L"3. 完成（已自动开启无线调试）",
                 adbOk ? L"正在用 ADB 转发音频，可以拔掉 USB 线了。"
               : anyWireless ? L"无线调试已开启。勾选设备列表里的设备即可开始转发。"
               : L"授权后程序会自动开启无线 ADB，之后不再需要插线。",
                 (adbOk || anyWireless) ? 2 : 0 };

    float py = card.Y + S(62);
    for (int i = 0; i < 3; i++)
    {
        RECTF rowR(px, py, pw, S(78));
        if (steps[i].state == 2) FillRound(g, rowR, S(8), ::g.th.SelectFill);
        else FillRound(g, rowR, S(8), ::g.th.Bg);

        // 状态圆点
        COLORREF c = (steps[i].state == 2) ? ::g.th.Ok
                   : (steps[i].state == 1) ? ::g.th.Warn
                                           : ::g.th.TrackBg;
        SolidBrush b(C(c));
        g.FillEllipse(&b, px + S(14), py + S(16), S(10), S(10));

        Text(g, steps[i].title, px + S(34), py + S(10), S(13.5f),
             steps[i].state == 2 ? ::g.th.Text : ::g.th.Text, steps[i].state == 2);
        TextWrap(g, steps[i].desc, px + S(34), py + S(32), pw - S(48), S(40), S(11.5f),
                 ::g.th.TextMuted);
        py += S(84);
    }

    // 底部按钮
    RECTF rHelp(px, card.GetBottom() - S(50), S(96), S(32));
    StrokeRound(g, rHelp, S(7), ::g.th.ControlBorder, 1);
    TextCenter(g, L"详细说明", rHelp, S(12.5f), ::g.th.TextMuted);
    ::g.hits.push_back({ ID_WIZ_HELP, -1, rHelp });

    RECTF rDone(card.GetRight() - S(28) - S(110), card.GetBottom() - S(50), S(110), S(32));
    FillRound(g, rDone, S(7), ::g.th.Accent);
    TextCenter(g, anyAuthorized ? L"完成" : L"稍后再说", rDone, S(13.f), ::g.th.OnAccent, true);
    ::g.hits.push_back({ ID_WIZ_CLOSE, -1, rDone });
}

#include "help_runtime.inc"

static void DrawHelp(Graphics& g, int w, int h)
{
    ::g.hits.clear();

    SolidBrush dim(Color(140, 0, 0, 0));
    g.FillRectangle(&dim, 0, 0, w, h);

    float cw = std::min(S(790), (float)w - S(24)), ch = S(470);
    RECTF card((w - cw) / 2, (h - ch) / 2, cw, ch);
    FillRound(g, card, S(12), ::g.th.ControlBg);
    StrokeRound(g, card, S(12), ::g.th.WindowBorder, 1);

    // 标题
    float px = card.X + S(24), py = card.Y + S(18);
    Text(g, L"帮助", px, py, S(16), ::g.th.Text, true);

    // 右上关闭
    RECTF rX(card.GetRight() - S(44), card.Y + S(12), S(32), S(28));
    if (::g.hover == ID_HELP_CLOSE) FillRound(g, rX, S(6), ::g.th.HoverFill);
    {
        Pen p(C(::g.th.TextMuted), S(1.2f));
        p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
        float hx = rX.X + S(16), hy = rX.Y + S(14), e = S(4);
        g.DrawLine(&p, hx - e, hy - e, hx + e, hy + e);
        g.DrawLine(&p, hx + e, hy - e, hx - e, hy + e);
    }
    ::g.hits.push_back({ ID_HELP_CLOSE, -1, rX });

    // 左侧目录
    struct { int topic; int id; const wchar_t* label; } items[] = {
        { HELP_CHANNEL, ID_HELP_NAV_CHANNEL, L"蓝牙 / ADB 通道" },
        { HELP_PAIR,    ID_HELP_NAV_PAIR,    L"配对与升级" },
        { HELP_VOLUME,  ID_HELP_NAV_VOLUME,  L"音量是怎么工作的" },
        { HELP_OUTPUT,  ID_HELP_NAV_OUTPUT,  L"输出设备优先级" },
        { HELP_FIXED,   ID_HELP_NAV_FIXED,   L"连接时切换输出" },
        { HELP_MULTI,   ID_HELP_NAV_MULTI,   L"多设备同时播放" },
        { HELP_STATUS,  ID_HELP_NAV_STATUS,  L"状态与图标含义" },
        { HELP_TROUBLE, ID_HELP_NAV_TROUBLE, L"常见问题" },
        { HELP_WHY,     ID_HELP_NAV_WHY,     L"为什么这样设计" },
        { HELP_RUNTIME, ID_HELP_NAV_RUNTIME, L"运行时文件" },
    };

    float navX = card.X + S(18), navY = card.Y + S(56), navW = S(180);
    for (auto& it : items)
    {
        bool active = (::g.helpTopic == it.topic);
        RECTF r(navX, navY, navW, S(30));
        if (active) FillRound(g, r, S(7), ::g.th.SelectFill);
        else if (::g.hover == it.id) FillRound(g, r, S(7), ::g.th.HoverFill);
        Text(g, it.label, navX + S(12), navY + S(7), S(12.5f),
             active ? ::g.th.Text : ::g.th.TextMuted, active, navW - S(18));
        ::g.hits.push_back({ it.id, -1, r });
        navY += S(32);
    }

    // 分隔线
    {
        Pen line(C(::g.th.Hairline), 1);
        g.DrawLine(&line, card.X + S(210), card.Y + S(56), card.X + S(210), card.GetBottom() - S(60));
    }

    // 右侧正文
    float bx = card.X + S(228), by = card.Y + S(56);
    float bw = cw - S(252);
    RECTF helpBodyHit(bx, by, bw + S(10), card.GetBottom() - S(88) - by);
    ::g.hits.push_back({ ID_HELP_BODY, -1, helpBodyHit });
    Text(g, HelpTitle(::g.helpTopic), bx, by, S(14.5f), ::g.th.Text, true);
    by += S(28);

    // 正文可滚动：先量总高度，超出则裁剪 + 滚轮滚动
    if (::g.helpTopic == HELP_RUNTIME)
    {
        // 内容比可视区高，需要裁剪 + 滚动（和普通正文一样）
        float bodyH = card.GetBottom() - S(88) - by;
        GraphicsState st0 = g.Save();
        g.SetClip(RectF(bx, by, bw, bodyH));
        float totalH = DrawRuntimeHelp(g, bx, by - ::g.helpScroll, bw);
        g.Restore(st0);

        float maxS = std::max(0.f, totalH - bodyH);
        ::g.helpScroll = std::clamp(::g.helpScroll, 0.f, maxS);

        if (maxS > 0.5f)
        {
            float thumbH = std::max(S(30), bodyH * bodyH / totalH);
            float t = (maxS > 0) ? (::g.helpScroll / maxS) : 0.f;
            RECTF track(bx + bw + S(6), by, S(6), bodyH);
            FillRound(g, track, S(3), ::g.th.Hairline);
            RECTF thumb(bx + bw + S(6), by + (bodyH - thumbH) * t, S(6), thumbH);
            FillRound(g, thumb, S(3), ::g.th.ControlBorder);
        }

        ::g.hits.push_back({ ID_HELP_BODY, -1, RectF(bx, by, bw + S(10), bodyH) });

        RECTF rDone3(card.GetRight() - S(24) - S(92), card.GetBottom() - S(18) - S(32), S(92), S(32));
        FillRound(g, rDone3, S(7), ::g.th.Accent);
        TextCenter(g, L"知道了", rDone3, S(13), ::g.th.OnAccent, true);
        ::g.hits.push_back({ ID_HELP_CLOSE, -1, rDone3 });
        return;
    }


    const wchar_t* body = HelpBody(::g.helpTopic);
    float bodyH = card.GetBottom() - S(88) - by;
    {
        // 固定行高逐字断行：GDI+ 的 MeasureString 对中英混排会高估约 9%，
        // 导致「滚到底还能多滚出几行空白」。这里量、画用同一份行数组，
        // 高度就是行数 × 行高，两头完全对齐。
        const float kLine = S(18);
        auto lines = WrapByWidth(g, body, bw, S(12.5f));
        float totalH = (float)lines.size() * kLine;
        float maxScroll = std::max(0.f, totalH - bodyH);
        ::g.helpScroll = std::clamp(::g.helpScroll, 0.f, maxScroll);

        GraphicsState st = g.Save();
        g.SetClip(RectF(bx, by, bw, bodyH));
        float ty = by - ::g.helpScroll;
        for (auto& ln : lines)
        {
            if (!ln.empty()) Text(g, ln, bx, ty, S(12.5f), ::g.th.TextMuted);
            ty += kLine;
        }
        g.Restore(st);

        // 滚动条（可滚动时才显示，宽度加大更明显）
        if (maxScroll > 0.5f)
        {
            float thumbH = std::max(S(30), bodyH * bodyH / totalH);
            float t = (maxScroll > 0) ? (::g.helpScroll / maxScroll) : 0.f;
            float thumbY = by + (bodyH - thumbH) * t;
            RECTF track(bx + bw + S(6), by, S(6), bodyH);
            FillRound(g, track, S(3), ::g.th.Hairline);
            RECTF thumb(bx + bw + S(6), thumbY, S(6), thumbH);
            FillRound(g, thumb, S(3), ::g.th.ControlBorder);
        }
    }

    // 标题栏空白处提示：? 可直达（放在「帮助」与 × 之间）
    {
        const wchar_t* hint = L"界面上各处的 ? 图标可直接跳到对应说明";
        float hx = px + S(52);
        float hw = rX.X - S(12) - hx;
        Font& hf = GetFont(S(11.5f), false);
        StringFormat sf;
        RectF mb;
        g.MeasureString(hint, (INT)wcslen(hint), &hf, SizeF(hw, S(60)), &sf, &mb);
        Text(g, hint, hx, card.Y + S(20), S(11.5f), ::g.th.TextFaint, false, hw);
    }

    // 底部按钮
    RECTF rDone(card.GetRight() - S(24) - S(92), card.GetBottom() - S(18) - S(32), S(92), S(32));
    FillRound(g, rDone, S(7), ::g.th.Accent);
    TextCenter(g, L"知道了", rDone, S(13), ::g.th.OnAccent, true);
    ::g.hits.push_back({ ID_HELP_CLOSE, -1, rDone });
}

// 小问号图标（可点击）
static void DrawHelpIcon(Graphics& g, float x, float y, int id)
{
    float d = S(15);
    RECTF r(x, y, d, d);
    bool hot = (::g.hover == id);
    FillRound(g, r, d / 2, hot ? ::g.th.AccentText : ::g.th.HoverFill);
    TextCenter(g, L"?", r, S(11), hot ? ::g.th.OnAccent : ::g.th.TextMuted, true);
    // 命中区放大一点，好点
    ::g.hits.push_back({ id, -1, RECTF(x - S(3), y - S(3), d + S(6), d + S(6)) });
}

// 缓冲挡位文案
static const wchar_t* kGearNames[4] = { L"激进", L"积极", L"标准", L"保守" };
// 挡位配色：保守=绿(稳) → 激进=红(险)
static COLORREF GearColor(int gear)
{
    switch (gear)
    {
    case 0: return RGB(0xD9, 0x5A, 0x4A);   // 激进 红
    case 1: return RGB(0xD9, 0x93, 0x3A);   // 积极 橙
    case 2: return RGB(0x3E, 0x7B, 0xD9);   // 标准 蓝
    default:return RGB(0x3E, 0xA8, 0x6A);   // 保守 绿
    }
}
static const wchar_t* kGearDesc[4] = {
    L"愿意牺牲一点流畅度，换取更低的缓冲和延迟。",
    L"紧贴缓冲，延迟很低，但可能出现卡顿。",
    L"留有一定缓冲空间，延迟与稳定性平衡，卡顿概率低。",
    L"留足缓冲空间，卡顿概率极低；出现卡顿一般属于特殊情况。"
};

static void DrawSettings(Graphics& g, int w, int h)
{
    ::g.hits.clear();

    SolidBrush dim(Color(140, 0, 0, 0));
    g.FillRectangle(&dim, 0, 0, w, h);

    float cw = S(500), ch = (std::min)(S(540), (float)h - S(16));   // 窗口矮时不能超出可视区
    RECTF card((w - cw) / 2, (h - ch) / 2, cw, ch);
    FillRound(g, card, S(12), ::g.th.ControlBg);
    StrokeRound(g, card, S(12), ::g.th.WindowBorder, 1);

    float px = card.X + S(22), pw = cw - S(44);

    // ---- 头部 ----
    Text(g, L"设置", px, card.Y + S(18), S(15), ::g.th.Text, true);
    RECTF rX(card.GetRight() - S(44), card.Y + S(12), S(32), S(28));
    if (::g.hover == ID_SET_DONE) FillRound(g, rX, S(6), ::g.th.HoverFill);
    {
        Pen p(C(::g.th.TextMuted), S(1.2f));
        p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
        float hx = rX.X + S(16), hy = rX.Y + S(14), e = S(4);
        g.DrawLine(&p, hx - e, hy - e, hx + e, hy + e);
        g.DrawLine(&p, hx + e, hy - e, hx - e, hy + e);
    }
    ::g.hits.push_back({ ID_SET_DONE, -1, rX });

    // ---- 主体（可滚动）----
    float headerH = S(42), footerH = S(54);
    RECTF body(card.X + S(1), card.Y + headerH, cw - S(2), ch - headerH - footerH);
    // 用上一帧的内容高度先把滚动夹紧，避免滚过头
    {
        float maxS = std::max(0.f, ::g.settingsContentH - body.Height + S(10));
        ::g.settingsScroll = std::clamp(::g.settingsScroll, 0.f, maxS);
    }

    float py = body.Y + S(4) - ::g.settingsScroll;
    float startY = py;

    GraphicsState cst = g.Save();
    g.SetClip(body);

    auto row = [&](float h) { py += h; };

    // 滚动后滚出可视区的行不能还留着命中区 —— 否则会盖住面板右上角的 ×，
    // 点 × 反而静默改了某条设置（实测过：点 × 把「收进托盘」改成了「直接退出」）。
    auto pushHit = [&](Hit h) {
        float x0 = (std::max)(h.r.X, body.X);
        float y0 = (std::max)(h.r.Y, body.Y);
        float x1 = (std::min)(h.r.GetRight(), body.GetRight());
        float y1 = (std::min)(h.r.GetBottom(), body.GetBottom());
        if (x1 - x0 > 0 && y1 - y0 > 0) ::g.hits.push_back({ h.id, h.index, RECTF(x0, y0, x1 - x0, y1 - y0) });
    };

    // ---- 关闭行为 ----
    Text(g, L"点关闭按钮时", px, py, S(12.5f), ::g.th.Text, true); row(S(24));
    {
        bool on = (::g.closeAction == L"tray");
        DrawRadio(g, px, py + S(1), on);
        Text(g, L"收进托盘，继续在后台工作", px + S(24), py, S(13.5f), ::g.th.Text);
        pushHit({ ID_SET_CLOSE_TRAY, -1, RECTF(px - S(4), py - S(5), pw, S(25)) });
        row(S(26));
        DrawRadio(g, px, py + S(1), !on);
        Text(g, L"直接退出程序", px + S(24), py, S(13.5f), ::g.th.Text);
        pushHit({ ID_SET_CLOSE_EXIT, -1, RECTF(px - S(4), py - S(5), pw, S(25)) });
        row(S(30));
    }

    // ---- 最小化行为 ----
    Text(g, L"点最小化按钮时", px, py, S(12.5f), ::g.th.Text, true); row(S(24));
    {
        bool on = (::g.minAction == L"tray");
        DrawRadio(g, px, py + S(1), on);
        Text(g, L"收进托盘", px + S(24), py, S(13.5f), ::g.th.Text);
        pushHit({ ID_SET_MIN_TRAY, -1, RECTF(px - S(4), py - S(5), pw, S(25)) });
        row(S(26));
        DrawRadio(g, px, py + S(1), !on);
        Text(g, L"留在任务栏（常规最小化）", px + S(24), py, S(13.5f), ::g.th.Text);
        pushHit({ ID_SET_MIN_TASKBAR, -1, RECTF(px - S(4), py - S(5), pw, S(25)) });
        row(S(30));
    }

    // ---- 开机自启的补充行为（主开关在主界面底栏）----
    {
        // 没开开机自启时这一项没有意义：画成未勾选 + 灰字，
        // 别再显示一个「看起来能点」的蓝勾框（点了没有任何反应）。
        DrawCheck(g, px, py, ::g.autoStart && ::g.silentStart, S(16));
        Text(g, L"自启后静默进入系统托盘", px + S(24), py, S(13.5f),
             ::g.autoStart ? ::g.th.Text : ::g.th.TextFaint);
        if (::g.autoStart)
            pushHit({ ID_SET_SILENT, -1, RECTF(px - S(4), py - S(5), pw, S(25)) });
        row(S(20));
        Text(g, ::g.autoStart ? L"开机自启已开启（可在主界面底栏切换）。"
                              : L"未开启开机自启；主界面底栏可打开。",
             px + S(24), py, S(11.5f), ::g.th.TextFaint, false, pw - S(24));
        row(S(26));
    }

    // ---- 无线设备重连 ----
    Text(g, L"无线连接断开后", px, py, S(12.5f), ::g.th.Text, true); row(S(24));
    DrawCheck(g, px, py, ::g.adb.LanScan(), S(16));
    Text(g, L"自动扫描局域网找回设备（IP 变化时）", px + S(24), py, S(13.5f), ::g.th.Text);
    pushHit({ ID_SET_LANSCAN, -1, RECTF(px - S(4), py - S(5), pw, S(25)) });
    row(S(22));
    TextWrap(g, L"关闭后：断线仍会自动重连同一 IP；IP 变了需重新插 USB 线。",
             px + S(24), py, pw - S(24), S(32), S(11.5f), ::g.th.TextMuted);
    row(S(34));

    DrawCheck(g, px, py, ::g.adb.AutoReconnect(), S(16));
    Text(g, L"自动重连", px + S(24), py, S(13.5f), ::g.th.Text);
    pushHit({ ID_SET_AUTORECONN, -1, RECTF(px - S(4), py - S(5), pw, S(25)) });
    row(S(22));
    TextWrap(g, L"关闭后：掉线就停在那儿，不再重连、也不再自动恢复"
                L"（启动后约两分钟内仍会照常连上）。"
                L"想停就关一下手机 WiFi；想用就打开 WiFi、点「刷新」。",
             px + S(24), py, pw - S(24), S(46), S(11.5f), ::g.th.TextMuted);
    row(S(48));

    // Android 11+ 无线调试配对：配对一次，手机重启也不用再插线
    {
        RECTF rPair(px, py, S(150), S(30));
        FillRound(g, rPair, S(7), ::g.hover == ID_PAIR_OPEN ? ::g.th.HoverFill : ::g.th.SelectFill);
        StrokeRound(g, rPair, S(7), ::g.th.ControlBorder, 1);
        TextCenter(g, L"无线调试配对…", rPair, S(12.5f), ::g.th.Text);
        pushHit({ ID_PAIR_OPEN, -1, rPair });
        Text(g, L"Android 11+，配对后手机重启也不用插线", px + S(160), py + S(8), S(11.5f), ::g.th.TextMuted);
    }
    row(S(36));

    // ---- 音频延迟 ----
    // 自动升级到 ADB
    DrawCheck(g, px, py, ::g.autoUpgrade, S(16));
    Text(g, L"蓝牙连上后自动升级到 ADB 音频", px + S(24), py, S(13.5f), ::g.th.Text);
    pushHit({ ID_SET_AUTOUP, -1, RECTF(px - S(4), py - S(5), pw, S(25)) });
    row(S(22));
    Text(g, L"默认约 220ms；调到「激进」约 80ms（有线约 40ms）。",
         px + S(24), py, S(11.5f), ::g.th.TextMuted, false, pw - S(24));
    row(S(30));

    Text(g, L"音频延迟", px, py, S(12.5f), ::g.th.Text, true); row(S(24));
    DrawCheck(g, px, py, ::g.adb.AutoAdapt(), S(16));
    Text(g, L"自适应缓冲（按实际卡顿自动调节）", px + S(24), py, S(13.5f), ::g.th.Text);
    pushHit({ ID_SET_AUTOADAPT, -1, RECTF(px - S(4), py - S(5), pw, S(25)) });
    row(S(22));
    Text(g, ::g.adb.AutoAdapt() ? L"开启：从预设起步，检测到丢样本后自动加大。"
                          : L"关闭：固定使用下面挡位的预设值。",
         px + S(24), py, S(11.5f), ::g.th.TextMuted, false, pw - S(24));
    row(S(26));

    // 闲时重置
    DrawCheck(g, px, py, ::g.idleReset, S(16));
    Text(g, L"空闲时再调整（设备没在播放时才重启）", px + S(24), py, S(13.5f), ::g.th.Text);
    pushHit({ ID_SET_IDLERESET, -1, RECTF(px - S(4), py - S(5), pw, S(25)) });
    row(S(22));
    Text(g, L"避免打断正在播放的音乐；预设模式下也生效（定期在空闲时重置通道）。",
         px + S(24), py, S(11.5f), ::g.th.TextMuted, false, pw - S(24));
    row(S(30));

    // 双通道热备
    DrawCheck(g, px, py, ::g.adb.DualChannel(), S(16));
    Text(g, L"双通道热备（实验性）", px + S(24), py, S(13.5f), ::g.th.Text);
    pushHit({ ID_SET_DUAL, -1, RECTF(px - S(4), py - S(5), pw, S(25)) });
    row(S(22));
    TextWrap(g, L"每台设备常驻两个通道，只有一路出声。需要换缓冲或出现卡顿时切到备用通道，"
                L"接管约 1.5 秒，且换缓冲时备用通道是静音的、不打断播放。"
                L"代价：设备端编码与网络占用约翻倍，平时可能偶发 0.5~1 秒小空档。",
             px + S(24), py, pw - S(24), S(34), S(11.5f), ::g.th.TextMuted);
    row(S(42));

    // 自动启用双通道
    DrawCheck(g, px, py, ::g.adb.AutoDual(), S(16));
    Text(g, L"卡顿频繁时自动启用双通道", px + S(24), py, S(13.5f), ::g.th.Text);
    pushHit({ ID_SET_AUTODUAL, -1, RECTF(px - S(4), py - S(5), pw, S(25)) });
    row(S(22));
    Text(g, L"5 分钟内需要调整 3 次以上，就自动打开双通道热备。",
         px + S(24), py, S(11.5f), ::g.th.TextMuted, false, pw - S(24));
    row(S(30));

    // 挡位滑条：最左「保守」→ 最右「激进」
    {
        const float trackX = px + S(14), trackW = pw - S(28);
        const float trackY = py + S(14), trackH = S(6);
        float posX[4];
        for (int i = 0; i < 4; i++) posX[i] = trackX + trackW * i / 3.f;

        int selDisplay = 3 - ::g.adb.BufferGear();      // 显示顺序与内部编号相反
        float kx = posX[selDisplay];

        // 轨道底色
        FillRound(g, RECTF(trackX, trackY, trackW, trackH), trackH / 2, ::g.th.Hairline);
        // 已选段（用当前挡位颜色）
        FillRound(g, RECTF(trackX, trackY, std::max(trackH, kx - trackX), trackH),
                  trackH / 2, GearColor(::g.adb.BufferGear()));

        // 四个停靠点
        for (int i = 0; i < 4; i++)
        {
            int gear = 3 - i;
            bool reached = (i <= selDisplay);
            float r = (i == selDisplay) ? S(9) : S(6);
            COLORREF c = reached ? GearColor(gear) : ::g.th.TrackBg;
            SolidBrush b(C(c));
            g.FillEllipse(&b, posX[i] - r / 2, trackY + trackH / 2 - r / 2, r, r);
        }
        // 滑块外圈
        {
            SolidBrush ring(C(::g.th.ControlBg));
            g.FillEllipse(&ring, kx - S(7), trackY + trackH / 2 - S(7), S(14), S(14));
            SolidBrush fill(C(GearColor(::g.adb.BufferGear())));
            g.FillEllipse(&fill, kx - S(4.5f), trackY + trackH / 2 - S(4.5f), S(9), S(9));
        }

        // 每个停靠点下面一行文字
        for (int i = 0; i < 4; i++)
        {
            int gear = 3 - i;
            bool on = (i == selDisplay);
            float tw = TextWidth(g, kGearNames[gear], S(11.5f), on);
            Text(g, kGearNames[gear], posX[i] - tw / 2, trackY + S(12), S(11.5f),
                 on ? GearColor(gear) : ::g.th.TextMuted, on);
            RECTF hit(posX[i] - S(26), trackY - S(14), S(52), S(46));
            pushHit({ ID_SET_GEAR_BASE + gear, -1, hit });
        }

        py = trackY + S(32);
        TextWrap(g, kGearDesc[::g.adb.BufferGear()], px, py, pw, S(32), S(11.5f), ::g.th.TextMuted);
        row(S(34));

        // 该挡位对应的预设缓冲（无线 / 有线）
        {
            wchar_t buf[128];
            swprintf_s(buf, L"预设：无线 %dms / 有线 %dms",
                       ::g.adb.GearDefaultMs(true), ::g.adb.GearDefaultMs(false));
            Text(g, buf, px, py, S(11.5f), ::g.th.TextFaint, false, pw);
            row(S(22));
        }
    }

    float contentH = py - startY;
    g.Restore(cst);
    ::g.settingsContentH = contentH;

    float maxScroll = std::max(0.f, contentH - body.Height + S(10));

    // ---- 底部：完成按钮单独一行 ----
    {
        float fy = card.GetBottom() - footerH;
        Pen line(C(::g.th.Hairline), 1);
        g.DrawLine(&line, card.X + S(12), fy, card.GetRight() - S(12), fy);

        RECTF rDone(card.GetRight() - S(22) - S(96), fy + S(14), S(96), S(32));
        FillRound(g, rDone, S(7), ::g.th.Accent);
        TextCenter(g, L"完成", rDone, S(13.5f), ::g.th.OnAccent, true);
        ::g.hits.push_back({ ID_SET_DONE, -1, rDone });

        // 版本号（左下角，免安装版标识）
        Text(g, L"WaveTether v" APP_VERSION L" · 免安装版",
             card.X + S(22), fy + S(20), S(11.f), ::g.th.TextFaint);
    }

    // 滚动条
    if (maxScroll > 0.5f)
    {
        float thumbH = std::max(S(30), body.Height * body.Height / (contentH + S(10)));
        float t = ::g.settingsScroll / maxScroll;
        float thumbY = body.Y + (body.Height - thumbH) * t;
        RECTF track(card.GetRight() - S(10), body.Y + S(4), S(5), body.Height - S(8));
        FillRound(g, track, S(2.5f), ::g.th.Hairline);
        RECTF thumb(card.GetRight() - S(10), thumbY, S(5), thumbH);
        FillRound(g, thumb, S(2.5f), ::g.th.ControlBorder);
    }

    ::g.settingsBody = body;
}

// 悬停提示：必须在所有内容之后绘制，否则会被卡片盖住
static void DrawTooltip(Graphics& g, int w)
{
    if (::g.settingsOpen) return;

    const wchar_t* tip = nullptr;
    float anchorCx = 0, anchorBottom = 0;
    // 与 DrawTitleBar 完全一致的布局：bx 是最左（主题）按钮的左边
    float bw = S(40), gap = S(4), by = S(8);
    float bx = (float)w - S(10) - bw * 3 - gap;
    if (::g.hover == ID_TITLE_THEME)       { tip = L"切换浅色 / 深色"; anchorCx = bx + bw / 2; }
    else if (::g.hover == ID_TITLE_MIN)    { tip = L"最小化";         anchorCx = bx + bw + gap + bw / 2; }
    else if (::g.hover == ID_TITLE_CLOSE)  { tip = L"关闭";           anchorCx = bx + (bw + gap) * 2 + bw / 2; }
    if (!tip) return;

    anchorBottom = by + S(28);
    float tw = TextWidth(g, tip, S(12.5f)) + S(20);
    float th = S(26);
    float tx = anchorCx - tw / 2;
    if (tx + tw > w - S(8)) tx = w - S(8) - tw;
    if (tx < S(8)) tx = S(8);
    RECTF box(tx, anchorBottom + S(6), tw, th);
    FillRound(g, box, S(6), ::g.th.ControlBg);
    StrokeRound(g, box, S(6), ::g.th.ControlBorder, 1);
    TextCenter(g, tip, box, S(12.5f), ::g.th.Text);
}

static void DrawCard(Graphics& g, RECTF r)
{
    FillRound(g, r, S(10), ::g.th.ControlBg);
}

// 统一的"链路是否可用"判定：用户主动断开后，即使系统里还残留手机音频会话，
// 也不能算已连接 —— 否则按钮永远显示"断开"，用户就没有重新连接的入口。
static bool LinkUp()
{
    // 只要任一链路在工作就算「已连接」——
    // 切到蓝牙标签页看列表时 ADB 仍在转发，状态不该变成「未连接」
    if (::g.adb.UiRunningCount() > 0) return true;
    if (::g.useAdb) return false;
    if (::g.bt.State() == LinkState::Connected) return true;
    if (::g.bt.UserDisconnected()) return false;
    return ::g.session.found;
}

static void DrawLeftColumn(Graphics& g, const Layout& L)
{
    // ---- 设备卡 ----
    DrawCard(g, L.devCard);
    float px = L.devCard.X + S(14), py = L.devCard.Y + S(12);
    float pw = L.devCard.Width - S(28);
    // 通道切换：蓝牙 / ADB
    {
        const wchar_t* tabs[] = { L"蓝牙", L"ADB 音频" };
        int ids[] = { ID_TAB_BT, ID_TAB_ADB };
        float tx = px;
        for (int i = 0; i < 2; i++)
        {
            bool active = (i == 1) == ::g.useAdb;
            float tw = TextWidth(g, tabs[i], S(12.5f), true) + S(20);
            RECTF r(tx, py - S(4), tw, S(26));
            if (active) FillRound(g, r, S(7), ::g.th.SelectFill);
            else if (::g.hover == ids[i]) FillRound(g, r, S(7), ::g.th.HoverFill);
            TextCenter(g, tabs[i], r, S(12.5f), active ? ::g.th.Text : ::g.th.TextMuted, active);
            ::g.hits.push_back({ ids[i], -1, r });
            tx += tw + S(4);
        }

        // 「勾选的设备上线后自动转发」是勾选框那一列的说明。
        // 原来画在 py - S(17)，正好落在标签胶囊的纵向范围内 → 压住「ADB 音频」。
        // 改成标签行右侧右对齐：不占额外高度，也不会再撞上标签。
        if (::g.useAdb)
        {
            const wchar_t* hint = L"勾选的设备上线后自动转发";
            float hw = TextWidth(g, hint, S(10.5f));
            // 窗口变窄时别把提示挤到标签上：放不下就不画（右侧 ▶/■ 的 tooltip 里有完整说明）
            if (px + pw - hw > tx + S(10)) Text(g, hint, px + pw - hw, py + S(2), S(10.5f), ::g.th.TextFaint);
        }
    }
    py += S(32);

    float btnH = S(30), btnY = L.devCard.GetBottom() - S(12) - btnH;
    float listH = btnY - py - S(10);

    // 列表区域（凹陷底）
    RECTF listR(px, py, pw, listH);
    FillRound(g, listR, S(8), ::g.th.Bg);
    ::g.hits.push_back({ ID_BT_LIST, -1, listR });

    float rowH = S(38);
    int visible = (int)((listH - S(6)) / rowH);
    auto rows = CurrentRows();
    int maxScroll = std::max(0, (int)rows.size() - visible);
    ::g.scrollDev = std::min(::g.scrollDev, maxScroll);

    for (int i = ::g.scrollDev; i < (int)rows.size() && i < ::g.scrollDev + visible; i++)
    {
        RECTF row(listR.X + S(3), listR.Y + S(3) + (i - ::g.scrollDev) * rowH, listR.Width - S(6), rowH);
        bool sel = ::g.useAdb ? (i == ::g.adbSel) : (i == ::g.btSel);
        RECTF iconHit{};
        RECTF autoBox{};
        RECTF btUpHit{};      // ↑ADB 按钮：必须在行命中之后登记，否则会被整行抢走
        if (sel) FillRound(g, row, S(7), ::g.th.SelectFill);
        else if (::g.hover == ID_BT_LIST && ::g.hoverIdx == i) FillRound(g, row, S(7), ::g.th.HoverFill);

        {
            const auto& it = rows[i];
            bool twoLine = !it.sub.empty();
            float ty = twoLine ? row.Y + S(4) : row.Y + (rowH - S(16)) / 2;
            // ADB 模式：左边一个「自动连接」勾选框
            if (::g.useAdb && it.ok && !it.serial.empty())
            {
                float cbs = S(15);
                float cbx = row.X + S(10), cby = row.Y + (rowH - cbs) / 2 - S(8);
                bool on = ::g.IsAutoDevice(it);
                DrawCheck(g, cbx, cby, on, cbs);
                ::g.hits.push_back({ ID_DEV_AUTO, i, RECTF(cbx - S(6), row.Y, cbs + S(14), rowH) });
                autoBox = RECTF(cbx - S(6), row.Y, cbs + S(14), rowH);
            }

            {
                float nameX = row.X + (::g.useAdb ? S(32) : S(12));
                if (::g.useAdb)
                {
                    Text(g, it.name, nameX, ty, S(13.5f), it.ok ? ::g.th.Text : ::g.th.TextMuted);
                }
                else
                {
                    // 右侧要给「状态文字 + ↑ADB 按钮」让位。原来固定预留 178px，
                    // 窗口拖到最小宽度时名字只剩约 5 个字，两台设备会显示成同一个名字。
                    // 现在按实际状态文字宽度算，并用省略号而不是硬截断。
                    std::wstring stGuess = it.btOnlyAdb ? L"仅 ADB"
                                         : !it.btPaired ? L"未配对"
                                         : (it.btState == 2) ? L"已连接 · 未用于音频"
                                         : (it.btState == 1) ? L"连接中…" : L"未连接";
                    float reserve = TextWidth(g, stGuess, S(11.f)) + S(16) + S(56);   // +56 给 ↑ADB
                    float nameW = row.GetRight() - nameX - reserve;
                    if (nameW < S(60)) nameW = S(60);
                    TextEllipsis(g, it.name, nameX, ty, nameW, S(13.5f),
                                 it.ok ? ::g.th.Text : ::g.th.TextMuted);
                }
            }

            if (twoLine)
            {
                float sy = ty + S(17);
                float sx = row.X + (::g.useAdb ? S(32) : S(12));
                const float subPx = S(11.5f);

                // 传输方式
                Text(g, it.sub, sx, sy, subPx, ::g.th.TextMuted);
                sx += TextWidth(g, it.sub, subPx) + S(7);

                // 两端状态：PC 端（scrcpy 在跑）/ 手机端（是否在出声）
                if (::g.useAdb && it.ok && !it.serial.empty())
                {
                    auto seg = [&](const wchar_t* label, bool on, bool last)
                    {
                        Text(g, label, sx, sy, subPx, ::g.th.TextMuted);
                        sx += TextWidth(g, label, subPx) + S(4);
                        SolidBrush db(C(on ? ::g.th.Ok : ::g.th.TrackBg));
                        g.FillEllipse(&db, sx, sy + S(4), S(6), S(6));
                        sx += S(6) + (last ? 0 : S(10));
                    };
                    seg(L"PC", it.running, false);
                    seg(L"设备", it.devPlaying, true);
                }
            }

            // 蓝牙模式：右侧右对齐「链路状态」+ 手动升级到 ADB
            if (!::g.useAdb)
            {
                std::wstring st;
                COLORREF stc = ::g.th.TextFaint;
                if (it.btOnlyAdb) { st = L"仅 ADB"; stc = ::g.th.TextMuted; }
                else if (!it.btPaired) { st = L"未配对"; stc = ::g.th.TextMuted; }
                else if (it.btState == 2)
                {
                    st = it.btAudio ? L"已连接 · 正在播放" : L"已连接 · 未用于音频";
                    stc = ::g.th.Ok;
                }
                else if (it.btState == 1) { st = L"连接中…"; stc = ::g.th.Warn; }
                else                      { st = L"未连接"; stc = ::g.th.TextFaint; }

                float right = row.GetRight() - S(10);

                // 升级到 ADB 不依赖蓝牙是否「已连接」：
                // A2DP 是按需连接的，没声音时 Windows 只保持配对，bt.State() 是断开态。
                // 用蓝牙名认设备就够，所以只要组件齐全、且这台还没在 ADB 跑，就给按钮。
                // 判「这台是不是已经在 ADB 转发了」有两个来源：
                //   ① 设备自己报的蓝牙名（d.btName）—— 不依赖任何学习过程，最可靠
                //   ② 应用层的 identity→蓝牙名 映射（btNames）—— 只在蓝牙真连上过才学到，
                //      A2DP 是按需连接，所以这台设备可能永远没有映射
                // 只用 ② 会导致「明明已在转发还显示 ↑ADB」。
                bool alreadyFwd = false, matchedAdb = false;
                for (auto& d : ::g.adb.UiDevices())
                {
                    if (d.state != "device") continue;
                    bool match = (!it.btMac.empty() && !d.btMac.empty() && d.btMac == it.btMac)
                              || (!d.btName.empty() && Utf8ToWide(d.btName) == it.name)
                              || (::g.NameForIdentity(d.identity) == it.name);
                    if (!match) continue;
                    matchedAdb = true;
                    if (::g.adb.UiIsRunning(d.serial)) { alreadyFwd = true; break; }
                }
                // 只给「已配对」或「能找到对应 ADB 设备」的行显示升级按钮。
                // 纯历史记录（设备早没了）点它必然失败，是死路，不如不显示。
                bool canUpgrade = ::g.adb.HasAdb() && ::g.adb.HasScrcpy() && ::g.adb.HasServer()
                               && !alreadyFwd && (it.btPaired || matchedAdb);
                if (canUpgrade)
                {
                    RECTF rg(right - S(46), row.Y + (rowH - S(22)) / 2, S(46), S(22));
                    bool hv = (::g.hover == ID_BT_UPGRADE && ::g.hoverIdx == i);
                    FillRound(g, rg, S(5), hv ? ::g.th.HoverFill : ::g.th.SelectFill);
                    TextCenter(g, L"↑ADB", rg, S(11.f), ::g.th.AccentText, true);
                    btUpHit = rg;
                    right = rg.X - S(8);
                }
                float tw2 = TextWidth(g, st, S(11.f));
                Text(g, st, right - tw2, row.Y + (rowH - S(14)) / 2, S(11.f), stc);
            }

            // ADB 模式：右侧逐设备开关（▶ 未转发 / ■ 转发中）
            if (::g.useAdb && it.ok && !it.serial.empty())
            {
                RECTF rg(row.GetRight() - S(40), row.Y, S(40), rowH);   // 整行高度，好点
                iconHit = rg;   // 图标命中区在行命中之后登记（后登记优先）
                if (::g.hover == ID_ADB_ROW && ::g.hoverIdx == i) FillRound(g, rg, S(5), ::g.th.HoverFill);
                float cx = rg.X + rg.Width / 2, cy = rg.Y + rg.Height / 2;
                if (it.running)
                {
                    SolidBrush b(C(::g.th.AccentText));
                    g.FillRectangle(&b, cx - S(4), cy - S(4), S(8), S(8));   // ■ 停止
                }
                else
                {
                    Point pts[3] = {
                        Point((INT)(cx - S(3)), (INT)(cy - S(5))),
                        Point((INT)(cx - S(3)), (INT)(cy + S(5))),
                        Point((INT)(cx + S(5)), (INT)cy)
                    };
                    SolidBrush b(C(::g.th.AccentText));
                    g.FillPolygon(&b, pts, 3);                                // ▶ 启动
                }
            }
        }
        ::g.hits.push_back({ rows[i].btEmptyHint ? ID_BT_OPEN_SETTINGS : ID_DEV_SELECT, i, row });   // 点行 = 选中该设备
        if (btUpHit.Width > 0) ::g.hits.push_back({ ID_BT_UPGRADE, i, btUpHit });  // ↑ADB 优先于整行
        if (iconHit.Width > 0) ::g.hits.push_back({ ID_ADB_ROW, i, iconHit });  // 图标优先
        if (autoBox.Width > 0) ::g.hits.push_back({ ID_DEV_AUTO, i, autoBox }); // 勾选框最优先
    }

    // 蓝牙模式仍需要「连接/断开」（没有其它入口）；ADB 模式用行内 ▶/■，按钮去掉
    if (!::g.useAdb)
    {
        // 只看蓝牙链路。不能复用 LinkUp()——它把「ADB 在后台转发」也算作已连接，
        // 会导致这里误显示「断开」，而点击只断蓝牙（本来就没连）→ 毫无反应。
        bool connected = (::g.bt.State() == LinkState::Connected);
        float cw = S(76);
        RECTF rConn(px, btnY, cw, btnH);
        bool hoverConn = (::g.hover == ID_BTN_CONNECT);
        if (connected)
        {
            FillRound(g, rConn, S(7), hoverConn ? ::g.th.HoverFill : ::g.th.SelectFill);
            TextCenter(g, L"断开", rConn, S(13.5f), ::g.th.Text);
        }
        else
        {
            FillRound(g, rConn, S(7), ::g.th.Accent, hoverConn ? (BYTE)210 : (BYTE)255);
            TextCenter(g, L"连接", rConn, S(13.5f), ::g.th.OnAccent, true);
        }
        ::g.hits.push_back({ ID_BTN_CONNECT, -1, rConn });

        RECTF rRefresh(px + cw + S(6), btnY, S(64), btnH);
        FillRound(g, rRefresh, S(7), ::g.hover == ID_BTN_REFRESH ? ::g.th.HoverFill : ::g.th.SelectFill);
        TextCenter(g, L"刷新", rRefresh, S(13.5f), ::g.th.Text);
        ::g.hits.push_back({ ID_BTN_REFRESH, -1, rRefresh });

        // 删除：清掉这台设备的全部痕迹（历史 / 勾选 / 音量 / 无线 IP）。
        // 破坏性操作 → 第一次点变成红色「确认删除」，3 秒内再点才执行。
        {
            RECTF rDel(px + cw + S(6) + S(64) + S(6), btnY, S(76), btnH);
            bool armed = (GetTickCount64() < ::g.deleteArmedUntil);
            bool hv = (::g.hover == ID_BTN_DELETE);
            if (armed)
            {
                FillRound(g, rDel, S(7), hv ? ::g.th.Err : ::g.th.Err);
                TextCenter(g, L"确认删除", rDel, S(12.5f), ::g.th.OnAccent, true);
            }
            else
            {
                FillRound(g, rDel, S(7), hv ? ::g.th.HoverFill : ::g.th.SelectFill);
                TextCenter(g, L"删除", rDel, S(13.5f), ::g.th.TextMuted);
            }
            ::g.hits.push_back({ ID_BTN_DELETE, -1, rDel });
        }
    }
    else
    {
        // ADB 模式：只显示提示（列表自动刷新，转发由行内 ▶/■ 控制）
        std::wstring hint = ::g.GetAdbHint();
        if (hint.empty()) hint = L"勾选的设备上线后自动转发；右侧 ▶/■ 立即启停";
        Text(g, hint, px, btnY + S(6), S(11.5f), ::g.th.TextFaint, false, pw);

        // 自适应延迟的学习结果
        if (::g.adb.AutoAdapt())
        {
            std::wstring buf;
            bool anyLearned = false;
            for (auto& ser : ::g.adb.UiRunningSerials())
            {
                if (!buf.empty()) buf += L" · ";
                int cur = ::g.adb.UiBufferMs(ser, 200);
                buf += std::to_wstring(cur) + L"ms";
                if (::g.adb.UiBufferIsLearned(ser)) { buf += L"✓"; anyLearned = true; }
                int d = ::g.adb.UiDrops(ser);
                if (d > 0) buf += L"（丢 " + std::to_wstring(d) + L"）";
            }
            if (!buf.empty())
                Text(g, L"延迟 " + buf, px, btnY + S(22), S(11.5f),
                     ::g.th.TextFaint, false, pw);
            if (anyLearned && ::g.adb.AutoAdapt())
                Text(g, L"✓ 已按实际使用学到更合适的值，下次启动沿用",
                     px, btnY + S(38), S(11.f), ::g.th.TextFaint, false, pw);
        }
    }

    // 状态卡已移除：连接状态在标题栏胶囊、播放状态在右栏、当前输出在设备列表「使用中」标签
}

static void DrawRightColumn(Graphics& g, const Layout& L)
{
    // ---- 手机声音 ----
    DrawCard(g, L.phoneCard);
    float px = L.phoneCard.X + S(14), py = L.phoneCard.Y + S(12);
    float pw = L.phoneCard.Width - S(28);

    // 单个模式下，标题带上设备名；该设备没在转发时明确提示
    bool singleNoSession = false;
    std::wstring devLabel;
    // 选中的是「当前页」的行：蓝牙页用 btSel，ADB 页用 adbSel。
    // 以前两个都先看 adbSel，而 adbSel 初值是 0 —— 蓝牙页会稳定显示第一台设备的名字。
    int singleIdx = ::g.useAdb ? ::g.adbSel : ::g.btSel;
    if (::g.volMode == 1)
    {
        if (auto* e = ::g.SelectedEntry()) devLabel = e->label;
        else
        {
            singleNoSession = true;
            auto rows = CurrentRows();
            if (singleIdx >= 0 && singleIdx < (int)rows.size()) devLabel = rows[singleIdx].name;
        }
    }
    float vol = singleNoSession ? 0.f : ::g.ActiveVolValue();
    bool vMute = singleNoSession ? false : ::g.ActiveMuteValue();

    // 没有音量会话时的状态词：标题和百分比共用一份，避免两处说法不一致。
    // 「未转发」是 ADB 的概念；蓝牙链路要区分「已连接但尚未出声」（会话枚举有延迟）
    // 和真的没连上，否则刚连上的一两秒会显示成「未连接」，像是连失败了。
    std::wstring noSessStatus;
    if (singleNoSession)
    {
        if (!::g.useAdb)
        {
            LinkState st = ::g.bt.State();
            noSessStatus = (st == LinkState::Connected) ? L"已连接 · 无音频"
                         : (st == LinkState::Connecting) ? L"连接中" : L"未连接";
        }
        else
        {
            auto rows = CurrentRows();
            bool offline = singleIdx >= 0 && singleIdx < (int)rows.size() &&
                           (rows[singleIdx].adbOffline || rows[singleIdx].sub == L"离线");
            noSessStatus = offline ? L"离线" : L"未转发";
        }
    }

    // 标题：全局=「全部设备」；单个=「设备名 (ADB/蓝牙)」
    std::wstring title;
    if (::g.volMode == 0) title = L"全部设备";
    else
    {
        title = devLabel;
        auto* e = ::g.SelectedEntry();
        if (e) title += e->isBt ? L"（蓝牙）" : L"（ADB）";
        else if (singleNoSession) title += L"（" + noSessStatus + L"）";
    }
    Text(g, title, px, py, S(12.5f), ::g.th.Text, true, pw - S(150));
    DrawHelpIcon(g, px + std::min(TextWidth(g, title, S(12.5f), true), pw - S(150)) + S(8), py + S(1), ID_HELP_Q_VOLUME);

    // 右侧：百分比 + 全局/单个 分段开关
    {
        float segW = S(50), segH = S(22), segGap = S(2);
        float segX = px + pw - (segW * 2 + segGap);
        float segY = py - S(4);

        struct { int id; const wchar_t* label; int mode; } segs[] = {
            { ID_VOL_GLOBAL, L"全局", 0 }, { ID_VOL_SINGLE, L"单个", 1 }
        };
        for (int i = 0; i < 2; i++)
        {
            RECTF r(segX + i * (segW + segGap), segY, segW, segH);
            bool on = (::g.volMode == segs[i].mode);
            if (on) FillRound(g, r, S(6), ::g.th.SelectFill);
            else if (::g.hover == segs[i].id) FillRound(g, r, S(6), ::g.th.HoverFill);
            TextCenter(g, segs[i].label, r, S(11.5f), on ? ::g.th.Text : ::g.th.TextMuted, on);
            ::g.hits.push_back({ segs[i].id, -1, r });
        }
    }
    py += S(26);

    // 滑块
    RECTF track(px, py + S(6), pw, S(3));
    FillRound(g, track, S(1.5f), ::g.th.TrackBg);
    float fx = track.X + track.Width * std::clamp(vol, 0.f, 1.f);
    FillRound(g, RECTF(track.X, track.Y, std::max(S(3), fx - track.X), S(3)), S(1.5f), ::g.th.Accent);
    {
        float tx = fx - S(8);
        SolidBrush bg(C(::g.th.Bg));
        g.FillEllipse(&bg, tx, py + S(6) - S(8) + S(1.5f) - S(1), S(16), S(16));
        SolidBrush fg(C(::g.th.Text));
        g.FillEllipse(&fg, tx + S(3), py + S(6) - S(8) + S(1.5f) - S(1) + S(3), S(10), S(10));
    }
    if (!singleNoSession)
    {
        RECTF sliderHit(px - S(8), py - S(6), pw + S(16), S(28));
        ::g.hits.push_back({ ID_SLIDER, -1, sliderHit });
    }
    py += S(34);

    // 静音 + 播放状态
    float cb = S(16);
    DrawCheck(g, px, py + S(1), vMute, cb);
    Text(g, L"静音该设备", px + cb + S(9), py, S(13.5f), ::g.th.Text);
    if (!singleNoSession)
    {
        RECTF muteHit(px - S(4), py - S(4), S(140), S(24));
        ::g.hits.push_back({ ID_MUTE, -1, muteHit });
    }

    // 音量大小放在这里（播放状态已移到标题栏）
    {
        wchar_t pct[32];
        if (singleNoSession) wcscpy_s(pct, noSessStatus.c_str());
        else swprintf_s(pct, L"%d%%", (int)(vol * 100 + 0.5f));
        Text(g, pct, px, py, S(12.5f), singleNoSession ? ::g.th.TextFaint : ::g.th.TextMuted, false, pw, true);
    }

    // ---- 输出设备 ----
    DrawCard(g, L.outCard);
    px = L.outCard.X + S(14);
    py = L.outCard.Y + S(12);
    pw = L.outCard.Width - S(28);
    Text(g, L"输出设备", px, py, S(12.5f), ::g.th.Text, true);
    DrawHelpIcon(g, px + TextWidth(g, L"输出设备", S(12.5f), true) + S(8), py + S(1), ID_HELP_Q_OUTPUT);
    py += S(26);

    DrawCheck(g, px, py, ::g.fixedMode, cb);
    Text(g, L"连接时切到优先级列表里的设备（断开自动还原）", px + cb + S(9), py, S(13.5f), ::g.th.Text);
    RECTF fixedHit(px - S(4), py - S(4), pw + S(8), S(24));
    ::g.hits.push_back({ ID_FIXED_OUT, -1, fixedHit });
    py += S(26);

    Text(g, L"不勾选 = 跟随系统默认输出，完全不改动系统设置。", px, py, S(12.5f), ::g.th.TextMuted);
    py += S(18);

    // 工具条
    struct { int id; const wchar_t* label; } tools[] = {
        { ID_MOVE_UP, L"上移" }, { ID_MOVE_DOWN, L"下移" },
        { ID_REFRESH_OUT, L"刷新设备" }, { ID_SOUND_SETTINGS, L"打开声音设置" }
    };
    float tx = px;
    for (auto& t : tools)
    {
        float w = TextWidth(g, t.label, S(13.5f)) + S(16);
        RECTF r(tx, py, w, S(30));
        if (::g.hover == t.id) FillRound(g, r, S(7), ::g.th.HoverFill);
        TextCenter(g, t.label, r, S(12.5f), ::g.hover == t.id ? ::g.th.AccentText : ::g.th.Text);
        ::g.hits.push_back({ t.id, -1, r });
        tx += w + S(4);
    }
    py += S(28);

    // 优先级列表
    float listBottom = L.outCard.GetBottom() - S(12);
    float listH = listBottom - py;
    RECTF listR(px, py, pw, std::max(S(34), listH));
    FillRound(g, listR, S(8), ::g.th.Bg);
    ::g.hits.push_back({ ID_PRIO_LIST, -1, listR });

    int total = (int)::g.prioOrder.size();
    float rowH = S(38);
    int visible = (int)((listR.Height - S(6)) / rowH);
    int maxScroll = std::max(0, total - visible);
    ::g.scrollPrio = std::min(::g.scrollPrio, maxScroll);

    int ord = 0;
    for (int i = ::g.scrollPrio; i < total && i < ::g.scrollPrio + visible; i++)
    {
        RECTF row(listR.X + S(3), listR.Y + S(3) + (i - ::g.scrollPrio) * rowH, listR.Width - S(6), rowH);
        bool sel = (i == ::g.prioSel);
        if (sel) FillRound(g, row, S(7), ::g.th.SelectFill);
        else if (::g.hover == ID_PRIO_LIST && ::g.hoverIdx == i) FillRound(g, row, S(7), ::g.th.HoverFill);

        bool on = (i < (int)::g.prioOn.size()) ? ::g.prioOn[i] : false;
        const OutDevice* od = FindOut(::g.prioOrder[i]);
        std::wstring nm = od ? od->name : ::g.prioOrder[i];
        bool act = od ? od->active : false;
        int myOrd = 0;
        if (on)
        {
            for (int k = 0; k <= i && k < (int)::g.prioOn.size(); k++) if (::g.prioOn[k]) myOrd++;
        }
        wchar_t ob[8];
        swprintf_s(ob, myOrd > 0 ? L"%d" : L"—", myOrd);
        Text(g, ob, row.X + S(8), row.Y + (rowH - S(16)) / 2, S(12.5f), ::g.th.TextMuted, true);

        DrawCheck(g, row.X + S(24), row.Y + (rowH - S(16)) / 2, on, cb);
        Text(g, nm, row.X + S(48), row.Y + (rowH - S(16)) / 2, S(13.5f),
             act ? ::g.th.Text : ::g.th.TextFaint, false, row.Width - S(120));

        bool inUse = !::g.currentDefaultId.empty() && ::g.prioOrder[i] == ::g.currentDefaultId;
        if (!inUse && act && ::g.hover == ID_PRIO_LIST && ::g.hoverIdx == i)
        {
            float hw = TextWidth(g, L"点击切换", S(11.5f)) + S(12);
            RECTF hint(row.GetRight() - hw - S(8), row.Y + (rowH - S(18)) / 2, hw, S(18));
            TextCenter(g, L"点击切换", hint, S(11.5f), ::g.th.AccentText);
        }
        if (inUse)
        {
            float bw2 = TextWidth(g, L"使用中", S(11.5f)) + S(12);
            RECTF badge(row.GetRight() - bw2 - S(12), row.Y + (rowH - S(18)) / 2, bw2, S(18));
            FillRound(g, badge, S(4), ::g.th.AccentSoft);
            TextCenter(g, L"使用中", badge, S(11.5f), ::g.th.AccentText);
        }
        ::g.hits.push_back({ ID_PRIO_LIST, i, row });
    }

}

static void DrawFooter(Graphics& g, const Layout& L, int w)
{
    RECTF f = L.footer;
    Pen line(C(::g.th.Hairline), 1);
    g.DrawLine(&line, f.X, f.Y, f.X + f.Width, f.Y);

    float px = S(24), py = f.Y + S(12), cb = S(16);
    struct { int id; const wchar_t* label; bool on; } items[] = {
        { ID_AUTOSTART, L"开机自启", ::g.autoStart },
        { ID_AUTOCONNECT, L"首次自动连蓝牙", ::g.autoConnect }
    };
    for (auto& it : items)
    {
        DrawCheck(g, px, py + S(1), it.on, cb);
        float tw = TextWidth(g, it.label, S(13.5f));
        Text(g, it.label, px + cb + S(9), py, S(13.5f), ::g.th.Text);
        ::g.hits.push_back({ it.id, -1, RECTF(px - S(4), py - S(4), cb + S(9) + tw + S(8), S(24)) });
        px += cb + S(9) + tw + S(22);
    }

    // 右侧：设置 / 日志 / 退出
    float rx = (float)w - S(24);
    // 从右往左排：设置固定在角落，常用的（刷新/自检）紧邻角落，低频的（退出/日志）在左端
    const wchar_t* labels[] = { L"设置", L"刷新", L"自检", L"帮助", L"打开日志", L"退出" };
    int ids[] = { ID_SETTINGS, ID_REFRESH, ID_CHECK, ID_HELP, ID_LOG, ID_QUIT };
    for (int i = 0; i < 6; i++)
    {
        float tw = TextWidth(g, labels[i], S(13.5f)) + S(18);
        rx -= tw;
        RECTF r(rx, py - S(3), tw, S(26));
        if (::g.hover == ids[i]) FillRound(g, r, S(7), ::g.th.HoverFill);
        TextCenter(g, labels[i], r, S(13.5f), ::g.hover == ids[i] ? ::g.th.AccentText : ::g.th.Text);
        ::g.hits.push_back({ ids[i], -1, r });
        rx -= S(4);
    }
}

static void RenderTo(HDC target, int w, int h)
{
    HWND hwnd = g_hwnd;
    if (w <= 0 || h <= 0) return;

    if (!::g.memDC || g.memW != w || g.memH != h)
    {
        if (::g.memBmp) { SelectObject(::g.memDC, ::g.oldBmp); DeleteObject(::g.memBmp); ::g.memBmp = nullptr; }
        if (::g.memDC) DeleteDC(::g.memDC);
        HDC screen = GetDC(hwnd);
        ::g.memDC = CreateCompatibleDC(screen);
        ::g.memBmp = CreateCompatibleBitmap(screen, w, h);
        ::g.oldBmp = (HBITMAP)SelectObject(::g.memDC, ::g.memBmp);
        ReleaseDC(hwnd, screen);
        g.memW = w;
        g.memH = h;
    }

    {
        Graphics gfx(::g.memDC);
        gfx.SetSmoothingMode(SmoothingModeAntiAlias);
        gfx.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        gfx.SetCompositingQuality(CompositingQualityHighQuality);

        SolidBrush bg(C(::g.th.Bg));
        gfx.FillRectangle(&bg, 0, 0, w, h);

        ::g.hits.clear();
        Layout L = ComputeLayout(w, h);

        DrawTitleBar(gfx, L);
        DrawLeftColumn(gfx, L);
        DrawRightColumn(gfx, L);
        DrawFooter(gfx, L, w);
        DrawTooltip(gfx, w);

        if (::g.settingsOpen) DrawSettings(gfx, w, h);
        if (::g.helpOpen) DrawHelp(gfx, w, h);
        if (::g.showWizard) DrawWizard(gfx, w, h);
        if (::g.pairOpen) DrawPairDialog(gfx, w, h);

        Pen edge(C(::g.th.WindowBorder), 1);
        gfx.DrawRectangle(&edge, 0, 0, w - 1, h - 1);
    }

    BitBlt(target, 0, 0, w, h, ::g.memDC, 0, 0, SRCCOPY);
}

// 临时诊断：记录慢帧，定位「切到 ADB 页卡一下」
static void Paint(HWND hwnd)
{
    PAINTSTRUCT ps2;
    HDC dc = BeginPaint(hwnd, &ps2);
    RECT rc;
    GetClientRect(hwnd, &rc);
    // 异常边界 + EndPaint 必须执行：BeginPaint 之后不 EndPaint，
    // 那块区域永远处于「待重绘」，会变成无限重绘 + CPU 打满。
    try { RenderTo(dc, rc.right, rc.bottom); }
    catch (const std::exception& e) { PerfLog("EXCEPTION Paint: %s", e.what()); }
    catch (...) { PerfLog("EXCEPTION Paint: unknown"); }
    EndPaint(hwnd, &ps2);
}

// ------------------------------------------------------------------ 自检窗口（独立窗口）
//
// 自检原来挂在帮助面板里当一个主题。但它不是「说明书」，而是「当前状态的诊断」，
// 混在帮助里既不好找，也要和帮助的滚动/关闭逻辑纠缠。改成独立窗口：
// 不影响主界面操作，也能单独拖到旁边对照着看。

static HWND  g_checkHwnd = nullptr;
static float g_checkScroll = 0.f;
static bool  g_checkHoverX = false;
static bool  g_checkTracking = false;
static const wchar_t* kCheckClass = L"WaveTetherSelfCheck";

static RECTF CheckCloseRect(float w)
{
    return RECTF(w - S(44), S(12), S(32), S(28));
}

static void PaintCheck(HWND hwnd)
{
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    // 绘制里抛异常也不能跳过 EndPaint：更新区会一直有效 → 无限 WM_PAINT，
    // 每帧还漏一个 memDC/位图。主窗口的 Paint 早就这么包了，这里漏了。
    struct PaintEndGuard
    {
        HWND h; PAINTSTRUCT* p;
        ~PaintEndGuard() { if (p) EndPaint(h, p); }
    } guard{ hwnd, &ps };
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;

    static HDC     memDC = nullptr;
    static HBITMAP memBmp = nullptr, oldBmp = nullptr;
    static int     mw = 0, mh = 0;
    if (!memDC || mw != w || mh != h)
    {
        if (memBmp) { SelectObject(memDC, oldBmp); DeleteObject(memBmp); memBmp = nullptr; }
        if (memDC) DeleteDC(memDC);
        HDC screen = GetDC(hwnd);
        memDC = CreateCompatibleDC(screen);
        memBmp = CreateCompatibleBitmap(screen, w, h);
        oldBmp = (HBITMAP)SelectObject(memDC, memBmp);
        ReleaseDC(hwnd, screen);
        mw = w; mh = h;
    }

    {
        Graphics gfx(memDC);
        gfx.SetSmoothingMode(SmoothingModeAntiAlias);
        gfx.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        gfx.SetCompositingQuality(CompositingQualityHighQuality);

        SolidBrush bg(C(::g.th.Bg));
        gfx.FillRectangle(&bg, 0, 0, w, h);

        // 标题栏
        Text(gfx, L"自检", S(24), S(14), S(16), ::g.th.Text, true);
        Text(gfx, L"当前状态诊断（只读，不会改动任何设置）", S(72), S(19), S(11.5f), ::g.th.TextMuted);

        // 关闭 ×
        RECTF rX = CheckCloseRect((float)w);
        if (g_checkHoverX) FillRound(gfx, rX, S(6), ::g.th.HoverFill);
        {
            Pen p(C(::g.th.TextMuted), S(1.2f));
            p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
            float cx = rX.X + rX.Width / 2, cy = rX.Y + rX.Height / 2, r = S(5);
            gfx.DrawLine(&p, cx - r, cy - r, cx + r, cy + r);
            gfx.DrawLine(&p, cx + r, cy - r, cx - r, cy + r);
        }

        // 内容：可滚动（自检项会随状态增减，多的时候会超出窗口）
        float padX = S(26), top = S(52), bottom = S(18);
        float bw = (float)w - padX * 2 - S(12);
        float bodyH = (float)h - top - bottom;
        if (bodyH < S(60)) bodyH = S(60);

        float totalH = 0.f;
        {
            GraphicsState stC = gfx.Save();
            gfx.SetClip(RectF(padX, top, bw + S(12), bodyH));
            totalH = DrawSelfCheck(gfx, padX, top - g_checkScroll, bw);
            gfx.Restore(stC);
        }

        float maxS = std::max(0.f, totalH - bodyH);
        g_checkScroll = std::clamp(g_checkScroll, 0.f, maxS);
        if (maxS > 0.5f)
        {
            float thumbH = std::max(S(30), bodyH * bodyH / totalH);
            float t = (maxS > 0) ? (g_checkScroll / maxS) : 0.f;
            RECTF track(padX + bw + S(6), top, S(6), bodyH);
            FillRound(gfx, track, S(3), ::g.th.Hairline);
            RECTF thumb(padX + bw + S(6), top + (bodyH - thumbH) * t, S(6), thumbH);
            FillRound(gfx, thumb, S(3), ::g.th.ControlBorder);
        }

        Pen edge(C(::g.th.WindowBorder), 1);
        gfx.DrawRectangle(&edge, 0, 0, w - 1, h - 1);
    }

    BitBlt(dc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    guard.p = nullptr;
}

static LRESULT CALLBACK CheckProcInner(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_NCCALCSIZE:
        if (wp) return 0;
        break;

    case WM_NCHITTEST:
    {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &p);
        RECT rc; GetClientRect(hwnd, &rc);
        RECTF rX = CheckCloseRect((float)rc.right);
        if (p.x >= rX.X && p.x <= rX.GetRight() && p.y >= rX.Y && p.y <= rX.GetBottom())
            return HTCLIENT;          // 关闭按钮要能点，不能被当成拖拽
        if (p.y < (int)S(44)) return HTCAPTION;
        return HTCLIENT;
    }

    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintCheck(hwnd); return 0;

    case WM_MOUSEWHEEL:
    {
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        g_checkScroll -= d * S(48) / WHEEL_DELTA;
        if (g_checkScroll < 0) g_checkScroll = 0;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc; GetClientRect(hwnd, &rc);
        RECTF rX = CheckCloseRect((float)rc.right);
        bool hv = (p.x >= rX.X && p.x <= rX.GetRight() && p.y >= rX.Y && p.y <= rX.GetBottom());
        if (hv != g_checkHoverX) { g_checkHoverX = hv; InvalidateRect(hwnd, nullptr, FALSE); }
        if (!g_checkTracking)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            g_checkTracking = TrackMouseEvent(&tme) != FALSE;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_checkTracking = false;
        if (g_checkHoverX) { g_checkHoverX = false; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;

    case WM_LBUTTONUP:
    {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc; GetClientRect(hwnd, &rc);
        RECTF rX = CheckCloseRect((float)rc.right);
        if (p.x >= rX.X && p.x <= rX.GetRight() && p.y >= rX.Y && p.y <= rX.GetBottom())
            ShowWindow(hwnd, SW_HIDE);
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { ShowWindow(hwnd, SW_HIDE); return 0; }
        break;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);      // 只隐藏，不销毁：下次打开更快，状态也留着
        return 0;

    case WM_DPICHANGED:
        // 跟随系统缩放重算尺寸（S() 读的是全局 dpi）
        if (g_checkHwnd)
        {
            RECT* r = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top,
                         (int)S(620), (int)S(560), SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 异常边界（自检窗口）：漏出去的异常会终止整个进程，把主窗口也带走。
static LRESULT CALLBACK CheckProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    try { return CheckProcInner(hwnd, msg, wp, lp); }
    catch (const std::exception& e) { PerfLog("EXCEPTION CheckProc msg=0x%04X: %s", msg, e.what()); }
    catch (...) { PerfLog("EXCEPTION CheckProc msg=0x%04X: unknown", msg); }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowCheckWindow()
{
    if (!g_checkHwnd)
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = CheckProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kCheckClass;
        RegisterClassExW(&wc);
        g_checkHwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kCheckClass, L"自检",
                                      WS_POPUP, 0, 0, (int)S(620), (int)S(560),
                                      g_hwnd, nullptr, wc.hInstance, nullptr);
        if (!g_checkHwnd) return;
    }

    int w = (int)S(620), h = (int)S(560);
    RECT mr{};
    GetWindowRect(g_hwnd, &mr);

    // 高度按内容自适应：自检项会随状态增减，固定高度会留一大片空白。
    // 先拿窗口 DC 量一遍（画上去也无所谓，紧接着就重绘）。
    {
        HDC dc = GetDC(g_checkHwnd);
        if (dc)
        {
            Graphics gfx(dc);
            float need = DrawSelfCheck(gfx, 0, 0, (float)w - S(26) * 2 - S(12));
            ReleaseDC(g_checkHwnd, dc);
            int fit = (int)(need + S(52) + S(18) + S(10));
            h = std::max((int)S(260), std::min(fit, (int)S(560)));
        }
    }

    int x = mr.left + ((mr.right - mr.left) - w) / 2;
    int y = mr.top + ((mr.bottom - mr.top) - h) / 2;

    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi))
    {
        x = std::max((int)mi.rcWork.left, std::min(x, (int)mi.rcWork.right - w));
        y = std::max((int)mi.rcWork.top, std::min(y, (int)mi.rcWork.bottom - h));
    }

    g_checkScroll = 0.f;
    SetWindowPos(g_checkHwnd, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW);
    InvalidateRect(g_checkHwnd, nullptr, FALSE);
    SetForegroundWindow(g_checkHwnd);
}

// ------------------------------------------------------------------ 行为

// 请求一次后台枚举（不阻塞 UI）。结果由 PollBtScan() 在 UI 线程取回。
static void RefreshDevices()
{
    if (::g.btScanBusy_.exchange(true)) return;      // 已有枚举在跑
    std::thread([]{
        auto list = ::g.bt.Enumerate();              // 纯静态查询，不碰 BtLink 内部状态
        {
            std::lock_guard<std::mutex> lk(::g.btScanMtx_);
            ::g.btScanResult_ = std::move(list);
        }
        ::g.btScanReady_ = true;
        ::g.btScanBusy_ = false;
    }).detach();
}

// UI 线程：把后台枚举结果取回来
static void PollBtScan()
{
    if (!::g.btScanReady_.exchange(false)) return;

    std::vector<SinkDevice> list;
    {
        std::lock_guard<std::mutex> lk(::g.btScanMtx_);
        list = ::g.btScanResult_;
    }
    ::g.btDevs = std::move(list);

    // 改名同步：同一个 MAC 出现新名字 → 直接覆盖，用户不用做任何事
    for (auto& d : ::g.btDevs)
    {
        if (d.mac.empty()) continue;
        auto it = ::g.btHistory.find(d.mac);
        if (it == ::g.btHistory.end() || it->second != d.name)
        {
            ::g.btHistory[d.mac] = d.name;
            ::g.btHistoryDirty = true;
        }

        // 老数据迁移：音量/静音以前按蓝牙名记，改成按 MAC（改名不丢）。只需做一次。
        {
            std::wstring oldKey = L"bt:" + d.name;
            std::wstring newKey = App::BtVolKey(d.name, d.mac);
            if (oldKey != newKey)
            {
                auto itv = ::g.devVolPref.find(oldKey);
                if (itv != ::g.devVolPref.end())
                {
                    if (!::g.devVolPref.count(newKey)) ::g.devVolPref[newKey] = itv->second;
                    ::g.devVolPref.erase(itv);
                    ::g.btHistoryDirty = true;
                }
                auto itm = ::g.devMutePref.find(oldKey);
                if (itm != ::g.devMutePref.end())
                {
                    if (!::g.devMutePref.count(newKey)) ::g.devMutePref[newKey] = itm->second;
                    ::g.devMutePref.erase(itm);
                    ::g.btHistoryDirty = true;
                }
            }
        }
    }

    if (::g.btSel >= (int)::g.btDevs.size()) ::g.btSel = (int)::g.btDevs.size() - 1;
    if (::g.btSel < 0 && !::g.btDevs.empty())
    {
        ::g.btSel = 0;
        for (size_t i = 0; i < ::g.btDevs.size(); i++)
            if (::g.btDevs[i].id == ::g.lastDeviceId) { ::g.btSel = (int)i; break; }
    }
}

static const OutDevice* FindOut(const std::wstring& id)
{
    for (auto& d : ::g.outs) if (d.id == id) return &d;
    return nullptr;
}

static void RefreshCurrentDefault()
{
    ::g.currentDefaultId = ::g.oa.DefaultRenderId();
    ::g.currentDefaultName = L"—";
    for (auto& d : ::g.outs)
        if (d.id == ::g.currentDefaultId) { ::g.currentDefaultName = d.name; break; }
}

static void RefreshOutputs()
{
    ::g.outs = ::g.oa.RenderDevices();
    // 重建 prioOn：prio 里的顺序在前，其余可用设备追加在后
    std::vector<std::wstring> order;
    std::vector<bool> on;
    for (auto& id : ::g.prio)
    {
        for (auto& d : ::g.outs)
            if (d.id == id) { order.push_back(id); on.push_back(true); break; }
    }
    for (auto& d : ::g.outs)
    {
        if (!d.active) continue;
        bool already = false;
        for (auto& id : order) if (id == d.id) { already = true; break; }
        if (!already) { order.push_back(d.id); on.push_back(false); }
    }
    // 勾选的排前面（保持各自顺序），未勾选的排后面
    std::vector<std::wstring> o2;
    std::vector<bool> n2;
    for (size_t i = 0; i < order.size(); i++) if (on[i]) { o2.push_back(order[i]); n2.push_back(true); }
    for (size_t i = 0; i < order.size(); i++) if (!on[i]) { o2.push_back(order[i]); n2.push_back(false); }
    order.swap(o2);
    on.swap(n2);

    ::g.prioOrder = order;
    ::g.prioOn = on;

    // 首次运行（无优先级配置）：默认把当前默认输出设备勾上
    bool anyOn = false;
    for (bool b : ::g.prioOn) if (b) { anyOn = true; break; }
    if (!anyOn)
    {
        auto cur = ::g.oa.DefaultRenderId();
        for (size_t i = 0; i < ::g.prioOrder.size(); i++)
            if (::g.prioOrder[i] == cur) { ::g.prioOn[i] = true; anyOn = true; break; }
        if (!anyOn)
        {
            for (size_t i = 0; i < ::g.prioOrder.size(); i++)
            {
                const OutDevice* od = FindOut(::g.prioOrder[i]);
                if (od && od->active) { ::g.prioOn[i] = true; break; }
            }
        }
        if (anyOn) SavePrio();
    }

    RefreshCurrentDefault();
}

static void SavePrio()
{
    ::g.prio.clear();
    for (size_t i = 0; i < ::g.prioOrder.size(); i++)
        if (i < ::g.prioOn.size() && ::g.prioOn[i]) ::g.prio.push_back(::g.prioOrder[i]);
    SaveSettings();
}

static void ApplyRouting()
{
    if (!::g.fixedMode) return;
    for (auto& id : ::g.prio)
    {
        for (auto& d : ::g.outs)
        {
            if (d.id == id && d.active)
            {
                auto cur = ::g.oa.DefaultRenderId();
                if (cur != id)
                {
                    ::g.routedPrev = cur;
                    if (::g.oa.SetDefaultRender(id))
                    {
                        ::g.routed = true;
                        // 立刻落盘：崩溃时靠这份记录还原（见 RecoverRoutingAfterCrash）
                        ::g.routeTarget = id;
                        ::g.routePrev = cur;
                        ::g.routeDirty = true;
                        SaveSettings();
                        // 系统默认输出被改掉了，必须告诉用户改到了哪、什么时候还原，
                        // 否则他只会发现「别的声音不见了」。
                        ::g.SetNotice(L"已把系统默认输出切到「" + d.name +
                                      L"」，断开连接后还原。不想自动切换可在设置里关掉「连接时切换输出」。", 0, 12000);
                    }
                }
                return;
            }
        }
    }
}

static void RestoreRouting()
{
    if (::g.routed && !::g.routedPrev.empty())
    {
        std::wstring name = ::g.routedPrev;
        for (auto& d : ::g.outs)
            if (d.id == ::g.routedPrev) { name = d.name; break; }

        bool ok = ::g.oa.SetDefaultRender(::g.routedPrev);
        ::g.routed = false;
        if (ok)
        {
            ::g.routeTarget.clear();
            ::g.routePrev.clear();
            ::g.routeDirty = false;      // 真的清掉（之前这里只写了注释、代码没做）
            ::g.SetNotice(L"已把默认输出还原为「" + name + L"」", 0, 8000);
        }
        else
        {
            // 还原失败：保留 dirty + target，让下次启动再试一次。
            // 否则「切走了没切回来」这个状态就再也无人处理了。
            PerfLog("ROUTE 正常断开时还原失败，留待下次启动重试");
            // 必须让用户看见：否则他只会觉得「整机突然没声音」，然后去修电脑。
            ::g.SetNotice(L"断开时没能把默认输出还原为「" + name +
                          L"」（设备可能已拔出）。请到右侧「输出设备」里点一下你在用的那个。", 1, 20000);
        }
        SaveSettings();
    }
}

// 启动时收拾「上次异常退出」的尾巴。
//
// 为什么需要：fixedMode 打开时程序会改 Windows 默认播放设备，正常断开才还原。
// 崩溃 / 强杀 / 断电后那个值就丢了，用户会「整机没声音且找不到原因」，
// 然后去修电脑 —— 不会想到是这个软件。
//
// 判定只用两个条件（不加「用户是否手动改过」的判断：那个没有可靠信号，
// 加了只会造出「已防住」的错觉）：
//   ① 上次是脏退出（settings 里 RouteDirty 还是 true）
//   ② 当前默认设备 == 当初我们切过去的那个
// 两条都成立才还原。用户手动改成别的 → 条件不成立，不动；
// 用户恰好手动改成同一个 → 会被误还原，代价是「再改一次」，
// 远小于「整机没声音且找不到原因」。
static void RecoverRoutingAfterCrash()
{
    if (!::g.routeDirty) return;

    std::wstring cur = ::g.oa.DefaultRenderId();
    bool weStillHold = (!::g.routeTarget.empty() && cur == ::g.routeTarget);

    // 无论还原不还原，标记都要清掉：否则下次启动还会再试一次
    ::g.routeDirty = false;

    if (weStillHold && !::g.routePrev.empty())
    {
        std::wstring name = ::g.routePrev;
        bool alive = false;
        for (auto& d : ::g.outs)
            // 必须是「存在的」端点，不只是「id 在列表里」：
            // RenderDevices 用 DEVICE_STATEMASK_ALL 枚举，已拔掉的耳机也在列表里（active=false）。
            // 还原到一个拔掉的端点 = 整机没声音，正好是本功能要避免的症状。
            if (d.id == ::g.routePrev && d.active) { alive = true; name = d.name; break; }

        if (alive && ::g.oa.SetDefaultRender(::g.routePrev))
        {
            // 必须可见：静默还原会让用户以为系统在乱动
            ::g.SetNotice(L"上次异常退出，已把默认输出还原为「" + name +
                          L"」。不想让程序自动切输出，可在设置里关掉「连接时切换输出」。", 1, 30000);
            PerfLog("ROUTE 崩溃恢复：已还原默认输出到 %s", PerfU8(name).c_str());
        }
        else
        {
            ::g.SetNotice(L"上次异常退出，默认输出可能还停在别的设备上。请到右侧「输出设备」里点一下你在用的那个。", 1, 15000);
            PerfLog("ROUTE 崩溃恢复：原设备已不存在或已拔出，放弃还原");
        }
    }
    // 这里**故意不调 SaveSettings**：
    // 此刻工作线程可能还没发布第一份快照，而 SaveSettings 会从快照读
    // KnownWireless / BufPref —— 快照为空就会把这两项永久写成空。
    // routeDirty 已在内存里置 false，下一次自然落盘（用户操作/退出）时带出去；
    // 即使这次没落盘，下次启动重跑一遍也是幂等的（还原成功后 cur != routeTarget，不会再动）。
}

// 这台蓝牙设备是否正在通过 ADB 转发。
// MAC 优先（改名不影响），MAC 拿不到时退回名字。
static bool BtDeviceForwarding(const std::wstring& name, const std::string& mac, std::string* serialOut = nullptr)
{
    for (auto& d : ::g.adb.UiDevices())
    {
        if (d.state != "device") continue;
        bool match = (!mac.empty() && !d.btMac.empty() && d.btMac == mac)
                  || (!d.btName.empty() && Utf8ToWide(d.btName) == name);
        if (!match) continue;
        if (::g.adb.UiIsRunning(d.serial)) { if (serialOut) *serialOut = d.serial; return true; }
    }
    return false;
}

// 删除某台蓝牙设备的全部记录。破坏性操作，调用方负责二次确认。
static void DeleteBtDevice()
{
    auto rows = CurrentRows();
    if (::g.btSel < 0 || ::g.btSel >= (int)rows.size()) return;
    auto& r = rows[::g.btSel];

    std::string serial, identity;
    for (auto& d : ::g.adb.UiDevices())
    {
        bool match = (!r.btMac.empty() && !d.btMac.empty() && d.btMac == r.btMac)
                  || (!d.btName.empty() && Utf8ToWide(d.btName) == r.name);
        if (!match) continue;
        serial = d.serial;
        identity = d.identity;
        break;
    }

    // 纯历史 / 纯 ADB 的行没有在线的 ADB 设备可匹配，identity 会是空的。
    // 这时按名字反查 btNames，否则 btNames/btMacs 里的条目会永远留着。
    if (identity.empty())
    {
        for (auto& kv : ::g.btNames)
            if (kv.second == r.name)
            {
                identity = WideToUtf8(kv.first);
                break;
            }
    }

    // 正在转发就先停掉，否则删完记录声音还在
    if (!serial.empty() && ::g.adb.UiIsRunning(serial))
    {
        std::string ser2 = serial;
        ::g.adb.Post([ser2]{ ::g.adb.StopDevice(ser2); });
    }

    // ① 蓝牙历史（按 MAC）
    if (!r.btMac.empty()) ::g.btHistory.erase(r.btMac);
    ::g.btHistoryDirty = true;

    // ② 勾选状态（按身份，兼容旧版按 serial）
    if (!identity.empty()) ::g.autoDevices.erase(identity);
    if (!serial.empty())   ::g.autoDevices.erase(serial);

    // ③ 音量 / 静音偏好
    ::g.devVolPref.erase(L"bt:" + r.name);
    ::g.devMutePref.erase(L"bt:" + r.name);
    if (!r.btMac.empty())
    {
        ::g.devVolPref.erase(L"bt:" + Utf8ToWide(r.btMac));
        ::g.devMutePref.erase(L"bt:" + Utf8ToWide(r.btMac));
    }
    if (!serial.empty())
    {
        ::g.devVolPref.erase(L"adb:" + Utf8ToWide(serial));
        ::g.devMutePref.erase(L"adb:" + Utf8ToWide(serial));
        if (!identity.empty())
        {
            ::g.devVolPref.erase(L"adb:" + Utf8ToWide(identity));
            ::g.devMutePref.erase(L"adb:" + Utf8ToWide(identity));
        }
    }

    // ④ 身份→蓝牙名 / MAC 映射
    if (!identity.empty())
    {
        ::g.btNames.erase(Utf8ToWide(identity));
        ::g.btMacs.erase(Utf8ToWide(identity));
    }

    // ⑤ 无线 IP 记录
    if (!serial.empty()) ::g.adb.ForgetWirelessAsync(serial);   // 异步：UI 线程不能等 mtx_

    // ⑥ 「上次连接的设备」也要清，否则下次启动还会自动去连已删的设备
    if (!r.name.empty() && ::g.lastDeviceName == r.name)
    {
        ::g.lastDeviceId.clear();
        ::g.lastDeviceName.clear();
    }

    // ⑦ 立墓碑：只对「没配对」的设备做。
    // 已配对的设备 Windows 每次枚举都会报出来，本来就会再出现（提示里已说明），
    // 给它立墓碑反而会让身份映射永远学不回来。
    if (!r.btPaired)
    {
        if (!r.btMac.empty()) ::g.btDeleted.insert(r.btMac);
        if (!identity.empty()) ::g.btDeleted.insert(identity);
    }

    ::g.btSel = -1;
    ::g.deleteArmedUntil = 0;
    SaveSettings();

    // 说明清楚删掉的是什么：程序记录 ≠ Windows 配对。
    // 已配对的设备删完记录还会从系统枚举里回来（这是对的，它确实还连着），
    // 所以要告诉用户想彻底移除该去哪里操作。
    if (r.btPaired)
        ::g.SetNotice(L"已清除程序内的记录。设备仍与 Windows 配对，所以还会显示；如需彻底移除请到系统设置里取消配对。", 0, 8000);
    else
        ::g.SetNotice(L"已删除这台设备的全部记录（重新勾选设备即可恢复）", 0, 5000);
}

static void ConnectSelected()
{
    // 注意：蓝牙页的列表是「历史 ∪ 枚举」合并后的结果，索引不能直接拿去查 btDevs
    auto rows = CurrentRows();
    if (::g.btSel < 0 || ::g.btSel >= (int)rows.size()) return;
    auto& r = rows[::g.btSel];

    if (!r.btPaired)
    {
        ::g.SetNotice(L"这台设备当前没有配对，请先在 Windows 蓝牙设置里配对", 1, 6000);
        return;
    }
    // 转发中的设备不允许再连蓝牙：两条链路同时出声会变双声
    if (BtDeviceForwarding(r.name, r.btMac))
    {
        ::g.SetNotice(L"该设备正在通过 ADB 转发，请先停止再连蓝牙（避免双声）", 1, 7000);
        return;
    }
    ::g.lastDeviceId = r.btId;
    ::g.lastDeviceName = r.name;
    SaveSettings();
    ::g.bt.Connect(r.btId, r.name);
}

static void ApplyAutoStart()
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (::g.autoStart)
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = std::wstring(L"\"") + exe + L"\"" + (::g.silentStart ? L" --tray" : L"");
        RegSetValueExW(k, L"WaveTether", 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        RegDeleteValueW(k, L"WaveTether");
    }
    RegCloseKey(k);
}

static HICON MakeTrayIcon(bool connected)
{
    // 16x16：白底深闪电（与标题栏图标同设计）
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, 16, 16);
    HBITMAP mask = CreateBitmap(16, 16, 1, 1, nullptr);
    auto old = SelectObject(dc, color);

    {
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        SolidBrush bg(C(RGB(0xF2, 0xF3, 0xF5)));
        g.FillRectangle(&bg, 0, 0, 16, 16);
        SolidBrush bolt(C(connected ? RGB(0x1F, 0x8A, 0x5E) : RGB(0x2A, 0x2E, 0x36)));
        PointF pts[6] = {
            PointF(9.6f, 2.0f), PointF(4.6f, 9.0f), PointF(7.6f, 9.0f),
            PointF(6.2f, 14.0f), PointF(11.4f, 6.8f), PointF(8.3f, 6.8f)
        };
        GraphicsPath p; p.AddPolygon(pts, 6);
        g.FillPath(&bolt, &p);
    }

    SelectObject(dc, old);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return ic;
}

static void UpdateTray()
{
    if (!::g.trayAdded) return;
    bool connected = LinkUp();
    static HICON lastIcon = nullptr;
    static bool lastConnected = false;
    if (!lastIcon || lastConnected != connected)
    {
        if (lastIcon) DestroyIcon(lastIcon);
        lastIcon = MakeTrayIcon(connected);
        lastConnected = connected;
    }
    ::g.nid.hIcon = lastIcon;

    // 只有图标/提示真正变化时才通知 explorer（跨进程调用，不能每 tick 都发）
    static bool lastState = false;
    if (lastIcon && lastState == connected && ::g.trayAdded) return;
    lastState = connected;

    lstrcpynW(::g.nid.szTip, connected ? L"WaveTether — 已连接" : L"WaveTether — 未连接", 128);
    Shell_NotifyIconW(NIM_MODIFY, &::g.nid);
}

// 用户显式唤起（托盘 / 二次启动 / 任务栏恢复）：显示并置顶，但不抢焦点。
// 与全局"点面板不抢焦点"保持一致 —— 想让它真正拿焦点，点面板即可。
static void ShowWindowFromTray()
{
    ShowWindow(g_hwnd, SW_SHOW);
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);

    // 允许 SetForegroundWindow 生效（否则被前台锁挡住）
    AllowSetForegroundWindow(ASFW_ANY);
    SetForegroundWindow(g_hwnd);
    BringWindowToTop(g_hwnd);
    SetFocus(g_hwnd);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void HideToTray()
{
    // 自检是独立窗口（owner = 主窗口），主窗口收进托盘时它也得跟着藏，
    // 否则会留一个孤零零的窗口在桌面上。
    if (g_checkHwnd && IsWindowVisible(g_checkHwnd)) ShowWindow(g_checkHwnd, SW_HIDE);
    ShowWindow(g_hwnd, SW_HIDE);
}

// ------------------------------------------------------------------ 日志导出（脱敏）
//
// 「打开日志」原来直接打开 %APPDATA%\WaveTether，那里躺着 settings.json（设备名、
// MAC、IP）和 adb.log（含配对地址）。而产品文案又是「把里面的文件发我」——
// 等于在引导用户泄露设备标识。
//
// 现在改成：导出到 logs-export\<时间>\，只放 perf.log，且导出前脱敏。
// 脱敏只动「已知的标识」：设备身份、MAC、IPv4 最后一段。
// 不用泛化的正则去猜，避免把正常的数字（毫秒、端口、字节数）也打掉。

static std::string ReplaceAllStr(std::string s, const std::string& from, const std::string& to)
{
    if (from.empty() || from == to) return s;
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos)
    {
        s.replace(p, from.size(), to);
        p += to.size();
    }
    return s;
}

// 把文本里所有 IPv4 的最后一段换成 ***（保留前三段，否则排障时看不出是不是同一个网段）
static std::string MaskIpv4(std::string s)
{
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size())
    {
        if (s[i] >= '0' && s[i] <= '9')
        {
            size_t j = i;
            int dots = 0;
            while (j < s.size() && ((s[j] >= '0' && s[j] <= '9') || s[j] == '.'))
            {
                if (s[j] == '.') ++dots;
                ++j;
            }
            std::string tok = s.substr(i, j - i);
            size_t lastDot = tok.find_last_of('.');
            bool looksIp = (dots == 3 && lastDot != std::string::npos &&
                            lastDot + 1 < tok.size() && tok.back() != '.');
            if (looksIp)
            {
                // 四段都必须是 1~3 位数字，否则可能只是「1.2.3.4.5」这种版本号
                bool ok = true;
                size_t p = 0;
                for (int seg = 0; seg < 4 && ok; ++seg)
                {
                    size_t q = tok.find('.', p);
                    if (q == std::string::npos) q = tok.size();
                    size_t len = q - p;
                    if (len == 0 || len > 3) ok = false;
                    p = q + 1;
                }
                if (ok) out += tok.substr(0, lastDot + 1) + "***";
                else out += tok;
            }
            else out += tok;
            i = j;
        }
        else out += s[i++];
    }
    return out;
}

static void ExportLogsAndOpen()
{
    wchar_t* appdata = nullptr;
    size_t n = 0;
    _wdupenv_s(&appdata, &n, L"APPDATA");
    std::wstring base = std::wstring(appdata ? appdata : L".") + L"\\WaveTether";
    free(appdata);

    // 带时间戳的子目录：多次导出不会互相覆盖，也不会把旧日志混进来
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stamp[64]{};
    swprintf_s(stamp, L"%04d%02d%02d-%02d%02d%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring exportRoot = base + L"\\logs-export";
    if (!CreateDirectoryW(exportRoot.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        ::g.SetNotice(L"导出失败：建不了 logs-export 目录", 2, 8000);
        return;
    }

    // 同一秒内连点两次也要能成功：目录已存在就加序号（之前直接失败）
    std::wstring outDir = exportRoot + L"\\" + stamp;
    for (int k = 2; k < 100 && !CreateDirectoryW(outDir.c_str(), nullptr); ++k)
    {
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
        outDir = exportRoot + L"\\" + stamp + L"-" + std::to_wstring(k);
    }
    if (GetFileAttributesW(outDir.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        ::g.SetNotice(L"导出失败：建不了本次导出目录", 2, 8000);
        return;
    }

    // 三段都要：perf.log.old 是轮转出去的上一段（问题往往就发生在轮转前后），
    // perf.log 是当前段，perf.log.1 是主文件被占用时的回落段。
    // 只发当前段等于把出事之前的记录丢掉。
    std::string text;
    const wchar_t* parts[] = { L"\\perf.log.old", L"\\perf.log", L"\\perf.log.1" };
    for (const wchar_t* part : parts)
    {
        std::wstring p = base + part;
        // 不存在的段直接跳过：ReadAll 会对打不开的文件写一行「读取失败」，
        // 那行会被后面的段读进去，变成导出日志里的一条假故障。
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        std::string seg = ReadAll(p);
        if (seg.empty()) continue;
        if (!text.empty() && text.back() != '\n') text += "\n";
        text += seg;
    }
    if (text.empty())
    {
        RemoveDirectoryW(outDir.c_str());        // 别留空目录
        ::g.SetNotice(L"还没有日志可导出（先复现一次问题再点「打开日志」）", 1, 8000);
        return;
    }

    // 1) 已知设备身份（含 mDNS 名字里的 serial）
    std::vector<std::string> ids;
    for (auto& id : ::g.autoDevices) ids.push_back(id);
    for (auto& d : ::g.adb.UiDevices())
    {
        if (!d.serial.empty()) ids.push_back(d.serial);
        if (!d.identity.empty()) ids.push_back(d.identity);
    }
    for (auto& id : ids)
    {
        if (id.size() < 6) continue;                 // 太短的不动，避免误伤普通单词
        std::string masked = id.substr(0, 3) + "***" + id.substr(id.size() - 2);
        text = ReplaceAllStr(text, id, masked);
    }

    // 1b) 历史设备：已删除/已换机的 serial 不在上面那份清单里，
    // 但它会以 mDNS 名字形式留在日志里（adb-<serial>-xxxxx._adb-tls-connect）。
    // 按形状脱敏，不依赖当前设备列表。
    {
        const std::string pre = "adb-";
        size_t p = 0;
        while ((p = text.find(pre, p)) != std::string::npos)
        {
            size_t start = p + pre.size();
            size_t dash = text.find('-', start);
            if (dash == std::string::npos) { p = start; continue; }
            std::string serial = text.substr(start, dash - start);
            bool looksSerial = serial.size() >= 6 && serial.size() <= 40;
            for (char c : serial)
                if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
                    looksSerial = false;
            if (looksSerial)
            {
                std::string masked = serial.substr(0, 3) + "***" + serial.substr(serial.size() - 2);
                text.replace(start, serial.size(), masked);
                p = start + masked.size();
            }
            else p = dash + 1;
        }
    }

    // 1c) 设备名字（用户在手机上起的名字，也是个人标识）
    {
        std::vector<std::string> names;
        for (auto& kv : ::g.btHistory) names.push_back(PerfU8(kv.second));
        for (auto& d : ::g.adb.UiDevices())
        {
            if (!d.btName.empty()) names.push_back(d.btName);
        }
        for (auto& nm : names)
        {
            if (nm.size() < 3) continue;
            text = ReplaceAllStr(text, nm, "<设备>");
        }
    }

    // 2) 已知 MAC（只处理紧凑形式 12 位十六进制 —— 这是 perf.log 里实际出现的写法；
    //    冒号/横线形式在日志里几乎不出现，而按「每 2 位加分隔符」去猜容易误伤时间串）
    std::vector<std::string> macs;
    for (auto& kv : ::g.btMacs) macs.push_back(PerfU8(kv.second));
    for (auto& kv : ::g.btHistory) macs.push_back(kv.first);
    for (auto& m : macs)
    {
        if (m.size() < 12) continue;
        std::string upper = m, lower = m;
        for (auto& c : upper) c = (char)toupper((unsigned char)c);
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        std::string masked = upper.substr(0, 6) + "******";   // 保留 OUI（前 3 段），后 3 段打掉
        text = ReplaceAllStr(text, upper, masked);
        text = ReplaceAllStr(text, lower, masked);
    }

    // 3) IPv4 最后一段
    text = MaskIpv4(text);

    // 4) 落盘
    std::wstring logOut = outDir + L"\\perf.log";
    FILE* f = nullptr;
    _wfopen_s(&f, logOut.c_str(), L"wb");
    if (!f)
    {
        ::g.SetNotice(L"导出失败：写不了文件", 2, 8000);
        return;
    }
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);

    const wchar_t* readme =
        L"这是 WaveTether 导出的诊断日志。\r\n\r\n"
        L"已做脱敏处理：\r\n"
        L"  · 设备序列号 / 设备标识：保留首 3 位与末 2 位，中间替换为 ***\r\n"
        L"  · 蓝牙 MAC：保留前 3 段（厂商 OUI），后 3 段替换为 ******\r\n"
        L"  · IPv4 地址：最后一段替换为 ***\r\n"
        L"\r\n"
        L"本目录只有 perf.log（含轮转出来的上一段），不含 settings.json，也不含 adb.log（后者可能带配对地址）。\r\n"
        L"如果仍有你不想发出的内容，可以用记事本打开直接删掉那几行。\r\n";
    {
        std::wstring rp = outDir + L"\\说明.txt";
        FILE* r = nullptr;
        _wfopen_s(&r, rp.c_str(), L"wb");
        if (r)
        {
            std::string u8 = PerfU8(readme);
            fwrite(u8.data(), 1, u8.size(), r);
            fclose(r);
        }
    }

    ShellExecuteW(nullptr, L"open", outDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ::g.SetNotice(L"已导出脱敏日志，把这个文件夹发给我就行", 0, 9000);
}

static void DoClick(int id, int idx)
{
    PerfScope psClick("DoClick", 3.0, id);
    switch (id)
    {
    case ID_TITLE_THEME:
        ::g.themeLight = !::g.themeLight;
        ::g.th = ::g.themeLight ? LightTheme() : DarkTheme();
        // 自检是独立窗口，主题变了它不会自己重绘
        if (g_checkHwnd) InvalidateRect(g_checkHwnd, nullptr, FALSE);
        SaveSettings();
        break;
    case ID_TITLE_MIN:
        if (::g.minAction == L"tray") HideToTray();
        else ShowWindow(g_hwnd, SW_MINIMIZE);
        return;
    case ID_TITLE_CLOSE:
        if (::g.closeAction == L"exit") { DestroyWindow(g_hwnd); return; }
        HideToTray();
        return;
    case ID_BTN_CONNECT:
        // 动作必须和标签一致：
        //   蓝牙页的按钮只管蓝牙链路；ADB 在后台转发时 LinkUp() 也为真，
        //   以前用 LinkUp() 判断会导致「标签写连接、点下去却在断开 ADB」。
        if (!::g.useAdb)
        {
            if (::g.bt.State() == LinkState::Connected) ::g.bt.Disconnect();
            else ConnectSelected();
            break;
        }
        if (LinkUp())
        {
            ::g.adb.Post([]{ ::g.adb.Stop(); });
        }
        else
        {
            // ADB：一次启动「所有」已授权设备 → 多设备同时播放
            // 探测和启动都是阻塞 IO，交给工作线程
            ::g.SetAdbHint(L"正在启动…");
            ::g.adb.Post([]{
                ::g.adb.Probe();
                int started = 0;
                // 注意：这里必须用真实列表 Devices()，不能用 UiDevices()——
                // 快照是在本任务结束后才刷新，此刻读快照会拿到旧数据
                for (auto& d : ::g.adb.Devices())
                {
                    if (d.state != "device" || d.serial.empty()) continue;
                    if (::g.adb.StartDevice(d.serial)) started++;
                }
                ::g.SetAdbHint((started == 0) ? std::wstring(L"未找到可启动的设备")
                            : (started == 1) ? std::wstring(L"ADB 音频转发中")
                                             : std::wstring(L"ADB 多设备同时转发中"));
            });
        }
        break;
    case ID_PAIR_OPEN:
        // 配对码是纯数字，开着中文输入法会把按键吃掉（WM_CHAR 收不到数字）
        ::g.savedImeCtx = ImmAssociateContext(g_hwnd, nullptr);
        ::g.pairOpen = true;
        // 手机上打开「使用配对码配对设备」时才会广播 _adb-tls-pairing，
        // 先查一次，能自动填地址就不用用户手打那个随机端口
        ::g.adb.RefreshPairingEndpointAsync();
        ::g.pairFocus = 0;
        ::g.pairStatusLocal.clear();
        break;
    case ID_PAIR_FIND:
        ::g.pairAddr.clear();
        ::g.pairStatusLocal = L"正在查找手机广播的配对地址…（手机上要停在「使用配对码配对设备」界面）";
        ::g.adb.RefreshPairingEndpointAsync();
        break;
    case ID_PAIR_ADDR: ::g.pairFocus = 0; break;
    case ID_PAIR_CODE: ::g.pairFocus = 1; break;
    case ID_PAIR_PORT: ::g.pairFocus = 2; break;
    case ID_PAIR_CANCEL:
        ImmAssociateContext(g_hwnd, ::g.savedImeCtx);
        // 取消要真的取消：否则后台那条 adb pair 会继续跑 20 秒，
        // 期间用户改参数重试会被「忙」挡掉，看起来像没反应。
        ::g.adb.CancelPairing();
        ::g.pairOpen = false;
        ::g.pairStatusLocal.clear();
        ::g.adb.ResetPairResult();
        break;
    case ID_PAIR_GO:
    {
        // 已成功 → 这颗按钮此时显示「完成」，点它就是关窗口
        if (!::g.adb.UiPairBusy() && ::g.adb.UiPairResult() == 1)
        {
            ImmAssociateContext(g_hwnd, ::g.savedImeCtx);
            ::g.pairOpen = false;
            ::g.pairStatusLocal.clear();
            ::g.adb.ResetPairResult();
            break;
        }
        if (::g.pairAddr.empty())
        {
            ::g.pairStatusLocal = L"请先填配对地址（手机「无线调试」页面上显示的那组 IP:端口）。";
            break;
        }
        // 配对码留空 = 这台已经配对过了，只要 adb connect 就行（mDNS 失效时的手动兜底）
        if (::g.pairCode.empty())
        {
            if (::g.pairAddr.find(L':') == std::wstring::npos)
            {
                ::g.pairStatusLocal = L"地址要写成 IP:端口，例如 192.168.1.100:37123（冒号不能漏）。";
                break;
            }
            std::string a2 = WideToUtf8(::g.pairAddr);
            ::g.adb.ResetUpgradeResult();
            ::g.adb.ConnectDirectAsync(a2);
            ::g.pairStatusLocal = L"正在连接…";
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        }
        if (::g.pairAddr.find(L':') == std::wstring::npos)
        {
            ::g.pairStatusLocal = L"配对地址要写成 IP:端口，例如 192.168.1.100:37123（冒号不能漏）。";
            break;
        }
        if (::g.pairCode.size() != 6)
        {
            ::g.pairStatusLocal = L"配对码是 6 位数字，请核对手机屏幕上显示的那一组。";
            break;
        }
        std::string a = WideToUtf8(::g.pairAddr);
        std::string c = WideToUtf8(::g.pairCode);
        std::string p = WideToUtf8(::g.pairPort);
        ::g.pairStatusLocal = L"正在配对…";
        ::g.adb.ResetPairResult();
        ::g.adb.PairWirelessAsync(a, c, p);
        break;
    }

    case ID_BTN_DELETE:
        // 二次确认：3 秒内连点两次才真的删
        if (GetTickCount64() < ::g.deleteArmedUntil) DeleteBtDevice();
        else
        {
            ::g.deleteArmedUntil = GetTickCount64() + 3000;
            ::g.SetNotice(L"再点一次「确认删除」将清除这台设备的全部记录", 1, 3000);
        }
        break;
    case ID_BTN_REFRESH:
        if (::g.useAdb) ::g.adb.Post([]{ ::g.adb.Probe(); });
        else RefreshDevices();
        break;
    case ID_TAB_BT:
        // 只切换界面，不停掉 ADB 转发 —— 否则"看一眼蓝牙列表"就把声音掐了。
        // 真正需要停 ADB 的时机是「蓝牙链路接管播放」时（见自动升级/连接逻辑）。
        ::g.useAdb = false;
        ::g.SetAdbHint(L"");
        RefreshDevices();
        break;
    case ID_TAB_ADB:
        ::g.useAdb = true;
        ::g.adb.Post([]{ ::g.adb.AdoptExternal(); ::g.adb.Probe(); });
        // 只有蓝牙真的在播时才断开它，避免两条链路同时出声
        if (::g.bt.State() == LinkState::Connected && ::g.adb.UiRunningCount() > 0)
            ::g.bt.Disconnect();
        ::g.SetAdbHint(::g.adb.HasAdb() ? L"已切到 ADB：插线即用，或先在设备上开启无线调试"
                                        : L"本版本没有 tools，ADB 不可用；用蓝牙即可");
        break;
    case ID_BT_LIST: if (idx >= 0) ::g.btSel = idx; break;
    case ID_DEV_SELECT:
        if (::g.useAdb) ::g.adbSel = idx; else ::g.btSel = idx;
        break;
    case ID_DEV_AUTO:
        // 用户重新勾选 = 明确要这台设备 → 解除之前的「已删除」标记
        {
            auto rows3 = CurrentRows();
            if (idx >= 0 && idx < (int)rows3.size())
            {
                if (!rows3[idx].btMac.empty()) ::g.btDeleted.erase(rows3[idx].btMac);
                for (auto& d : ::g.adb.UiDevices())
                {
                    bool match = (!rows3[idx].btMac.empty() && d.btMac == rows3[idx].btMac)
                              || (!d.btName.empty() && Utf8ToWide(d.btName) == rows3[idx].name);
                    if (match && !d.identity.empty()) ::g.btDeleted.erase(d.identity);
                }
            }
        }
    {
        auto rows = CurrentRows();
        if (idx >= 0 && idx < (int)rows.size() && !rows[idx].serial.empty())
        {
            const std::string key = rows[idx].identity.empty() ? rows[idx].serial : rows[idx].identity;
            if (::g.autoDevices.count(key)) ::g.autoDevices.erase(key);
            else ::g.autoDevices.insert(key);
            ::g.userStopped.erase(rows[idx].serial);   // 重新勾选后允许自动拉起
            SaveSettings();
        }
        break;
    }
    case ID_ADB_ROW:
    {
        auto rows = CurrentRows();
        if (idx >= 0 && idx < (int)rows.size() && !rows[idx].serial.empty())
        {
            const std::string ser = rows[idx].serial;
            if (::g.adb.UiIsRunning(ser))
            {
                ::g.userStopped.insert(ser);          // 用户主动停的，别再自动拉起
                ::g.adb.Post([ser]{ ::g.adb.StopDevice(ser); });
                ::g.SetAdbHint(L"已停止该设备");
            }
            else
            {
                ::g.userStopped.erase(ser);
                ::g.adb.Post([ser]{ ::g.adb.StartDevice(ser); ::g.adb.Probe(); });
                ::g.SetAdbHint(L"已开始转发");
            }
        }
        break;
    }
    case ID_MUTE:
    {
        bool m = !::g.ActiveMuteValue();
        ::g.SetActiveMute(m);
        if (::g.volMode == 0) ::g.ApplyMuteAll(m);
        else
        {
            auto k = ::g.SelectedKey();
            for (auto& e : ::g.devVols)
                if (e.key == k) { ::g.oa.SetSessionMute(e.instanceId, m); break; }
        }
        SaveSettings();
        break;
    }
    case ID_VOL_GLOBAL: ::g.volMode = 0; SaveSettings(); break;
    case ID_VOL_SINGLE:
        ::g.volMode = 1;
        // 当前选中的设备没在转发 → 自动跳到一台有会话的
        if (!::g.SelectedEntry())
        {
            auto rows = CurrentRows();
            for (int i = 0; i < (int)rows.size(); i++)
            {
                std::wstring key = ::g.useAdb ? (L"adb:" + Utf8ToWide(rows[i].serial))
                                              : App::BtVolKey(rows[i].name, rows[i].btMac);
                if (::g.FindDevVol(key))
                {
                    if (::g.useAdb) ::g.adbSel = i; else ::g.btSel = i;
                    break;
                }
            }
        }
        SaveSettings();
        break;
    case ID_FIXED_OUT:
        ::g.fixedMode = !::g.fixedMode;
        if (::g.fixedMode) ApplyRouting(); else RestoreRouting();
        SaveSettings();
        break;
    case ID_PRIO_LIST: ::g.prioSel = idx; break;
    case ID_MOVE_UP:
        // 列表可能因设备插拔变短，必须同时检查上下界
        if (::g.prioSel > 0 &&
            ::g.prioSel < (int)::g.prioOrder.size() &&
            ::g.prioSel < (int)::g.prioOn.size())
        {
            std::swap(::g.prioOrder[::g.prioSel], ::g.prioOrder[::g.prioSel - 1]);
            // prioOn 是 std::vector<bool>，元素是代理对象：std::swap 会「先覆盖再读」
            // （实测 {1,0}→{0,0}、{0,1}→{1,1}）。用 pure.h 里的 SwapBoolAt。
            SwapBoolAt(::g.prioOn, (size_t)::g.prioSel, (size_t)::g.prioSel - 1);
            ::g.prioSel--;
            SavePrio();
        }
        break;
    case ID_MOVE_DOWN:
        if (::g.prioSel >= 0 && ::g.prioSel + 1 < (int)::g.prioOrder.size())
        {
            std::swap(::g.prioOrder[::g.prioSel], ::g.prioOrder[::g.prioSel + 1]);
            SwapBoolAt(::g.prioOn, (size_t)::g.prioSel, (size_t)::g.prioSel + 1);
            ::g.prioSel++;
            SavePrio();
        }
        break;
    case ID_REFRESH_OUT: RefreshOutputs(); break;
    case ID_SOUND_SETTINGS:
        ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case ID_AUTOSTART: ::g.autoStart = !::g.autoStart; SaveSettings(); ApplyAutoStart(); break;
    case ID_AUTOCONNECT: ::g.autoConnect = !::g.autoConnect; SaveSettings(); break;
    case ID_QUIT: DestroyWindow(g_hwnd); return;
    case ID_LOG:
        ExportLogsAndOpen();
        break;
    case ID_SETTINGS: ::g.settingsOpen = true; ::g.settingsScroll = 0.f; break;
    case ID_REFRESH:
    {
        // 刷新：ADB 重新探测设备，蓝牙重新枚举；已转发的设备保持不动
        //
        // 「自动重连」关掉后，这里就是「重新连上」的唯一入口：
        // 立刻试一次已知无线地址（不等后台轮询、不受启动窗口限制）。
        ::g.adb.Post([]{ ::g.adb.Probe(); ::g.adb.ReconnectKnownWireless(true); });
        RefreshDevices();
        ::g.SetAdbHint(L"已刷新");
        break;
    }
    case ID_CHECK:
        ShowCheckWindow();
        break;
    case ID_HELP: ::g.helpOpen = true; if (::g.helpTopic == HELP_NONE) ::g.helpTopic = HELP_CHANNEL;
        ::g.helpScroll = 0; break;
    case ID_HELP_CLOSE: ::g.helpOpen = false; break;
    case ID_HELP_NAV_CHANNEL: ::g.helpTopic = HELP_CHANNEL; ::g.helpScroll = 0; break;
    case ID_HELP_NAV_PAIR:    ::g.helpTopic = HELP_PAIR;    ::g.helpScroll = 0; break;
    case ID_HELP_NAV_VOLUME:  ::g.helpTopic = HELP_VOLUME; ::g.helpScroll = 0; break;
    case ID_HELP_NAV_OUTPUT:  ::g.helpTopic = HELP_OUTPUT; ::g.helpScroll = 0; break;
    case ID_HELP_NAV_FIXED:   ::g.helpTopic = HELP_FIXED; ::g.helpScroll = 0; break;
    case ID_HELP_NAV_MULTI:   ::g.helpTopic = HELP_MULTI; ::g.helpScroll = 0; break;
    case ID_HELP_NAV_STATUS:  ::g.helpTopic = HELP_STATUS; ::g.helpScroll = 0; break;
    case ID_HELP_NAV_TROUBLE: ::g.helpTopic = HELP_TROUBLE; ::g.helpScroll = 0; break;
    case ID_HELP_NAV_WHY:     ::g.helpTopic = HELP_WHY;     ::g.helpScroll = 0; break;
    case ID_HELP_NAV_RUNTIME: ::g.helpTopic = HELP_RUNTIME; ::g.helpScroll = 0; break;
    case ID_HELP_Q_VOLUME: ::g.helpOpen = true; ::g.helpTopic = HELP_VOLUME; ::g.helpScroll = 0; break;
    case ID_HELP_Q_OUTPUT: ::g.helpOpen = true; ::g.helpTopic = HELP_OUTPUT; ::g.helpScroll = 0; break;
    case ID_SET_CLOSE_TRAY: ::g.closeAction = L"tray"; SaveSettings(); break;
    case ID_SET_CLOSE_EXIT: ::g.closeAction = L"exit"; SaveSettings(); break;
    case ID_SET_MIN_TRAY: ::g.minAction = L"tray"; SaveSettings(); break;
    case ID_SET_MIN_TASKBAR: ::g.minAction = L"taskbar"; SaveSettings(); break;
    case ID_SET_IDLERESET:
        ::g.idleReset = !::g.idleReset;
        SaveSettings();
        break;
    case ID_SET_AUTODUAL:
        ::g.adb.SetAutoDual(!::g.adb.AutoDual());
        SaveSettings();
        break;
    case ID_BT_OPEN_SETTINGS:
        ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
        break;

    case ID_BT_UPGRADE:
    {
        // 手动升级：复用已有的异步请求路径（工作线程去做 adb 那套，UI 不阻塞）
        auto rows2 = CurrentRows();
        if (idx >= 0 && idx < (int)rows2.size())
        {
            if (!rows2[idx].btMac.empty()) ::g.btDeleted.erase(rows2[idx].btMac);
            ::g.adb.ResetUpgradeResult();
            ::g.adb.RequestUpgradeForBt(rows2[idx].name, rows2[idx].btMac);
            ::g.SetNotice(L"正在升级到 ADB 音频…", 0, 4000);
        }
        break;
    }

    case ID_SET_AUTOUP:
        ::g.autoUpgrade = !::g.autoUpgrade;
        SaveSettings();
        break;

    case ID_SET_AUTORECONN:
        ::g.autoReconnect = !::g.autoReconnect;
        ::g.adb.SetAutoReconnect(::g.autoReconnect);   // atomic，UI 线程直接写安全
        SaveSettings();
        break;

    case ID_SET_DUAL:
        // 走工作线程：SetDualChannel 要锁 mtx_，而工作线程做 adb IO 时可能持锁好几秒，
        // 直接在 UI 线程调会把窗口冻住。
        {
            bool on = !::g.adb.DualChannel();
            ::g.adb.Post([on]{ ::g.adb.SetDualChannel(on); });
        }
        SaveSettings();
        break;
    case ID_WIZ_CLOSE:
        ::g.showWizard = false;
        ::g.setupDone = true;
        // 顺手把已授权的设备勾上，用户不用再去找那个方框
        for (auto& d : ::g.adb.UiDevices())
        {
            if (d.state != "device") continue;
            ::g.autoDevices.insert(d.identity.empty() ? d.serial : d.identity);
            ::g.useAdb = true;          // 切到 ADB 模式，自动转发才会跑起来
        }
        SaveSettings();
        break;
    case ID_WIZ_HELP:
        ::g.showWizard = false;
        ::g.helpOpen = true;
        ::g.helpTopic = HELP_CHANNEL;
        ::g.helpScroll = 0;
        break;
    case ID_SET_SILENT:
        ::g.silentStart = !::g.silentStart;
        SaveSettings();
        ApplyAutoStart();
        break;
    case ID_SET_GEAR_BASE: case ID_SET_GEAR_1: case ID_SET_GEAR_2: case ID_SET_GEAR_3:
    {
        int g = id - ID_SET_GEAR_BASE;
        if (g != ::g.adb.BufferGear())
        {
            ::g.adb.SetBufferGear(g);
            // 换挡后清掉已学到的值，按新挡位重新起步
            ::g.adb.SetBufferPrefs({});
            SaveSettings();
            // 换挡要让新缓冲生效，但必须走「延迟重启」：
            // 立刻重启会导致设备端还没释放音频输入，新会话抢不到声音。
            ::g.adb.Post([]{ ::g.adb.ApplyGearToRunning(); });
        }
        break;
    }
    case ID_SET_AUTOADAPT:
        ::g.adb.SetAutoAdapt(!::g.adb.AutoAdapt());
        SaveSettings();
        break;
    case ID_SET_LANSCAN:
        ::g.adb.SetLanScan(!::g.adb.LanScan());
        SaveSettings();
        break;
    case ID_SET_DONE: ::g.settingsOpen = false; break;
    default: break;
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 点击列表里的复选框区域（序号右侧那块）
// 手动切换：把列表里这一行的设备设为当前系统输出
static void SwitchOutputTo(int idx)
{
    if (idx < 0 || idx >= (int)::g.prioOrder.size()) return;
    const auto& id = ::g.prioOrder[idx];
    const OutDevice* od = FindOut(id);
    if (!od || !od->active) return;                 // 不可用的设备不能切

    if (::g.oa.SetDefaultRender(id))
    {
        ::g.routedPrev = ::g.currentDefaultId;      // 记住切换前，供断开还原
        ::g.routed = true;
        RefreshCurrentDefault();
        ::g.prioSel = idx;
    }
}

static void TogglePrioCheck(int idx)
{
    if (idx < 0 || idx >= (int)::g.prioOn.size()) return;
    ::g.prioOn[idx] = !::g.prioOn[idx];

    // 重排：勾选的在前
    std::vector<std::wstring> o;
    std::vector<bool> n;
    for (size_t i = 0; i < ::g.prioOrder.size(); i++)
        if (i < ::g.prioOn.size() && ::g.prioOn[i]) { o.push_back(::g.prioOrder[i]); n.push_back(true); }
    for (size_t i = 0; i < ::g.prioOrder.size(); i++)
        if (!(i < ::g.prioOn.size() && ::g.prioOn[i])) { o.push_back(::g.prioOrder[i]); n.push_back(false); }
    ::g.prioOrder.swap(o);
    ::g.prioOn.swap(n);
    ::g.prioSel = -1;
    SavePrio();
}

// ------------------------------------------------------------------ 消息

// 真正的消息处理。外面包一层 WndProc 做异常边界，见下。
static LRESULT CALLBACK WndProcInner(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_GETMINMAXINFO:
    {
        // 布局是按 960x556 设计的，再小控件会互相压住
        auto* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = (LONG)(760 * ::g.dpi);
        mmi->ptMinTrackSize.y = (LONG)(500 * ::g.dpi);
        return 0;
    }
    case WM_NCCALCSIZE:
        if (wp) return 0;
        break;

    case WM_NCHITTEST:
    {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &p);
        RECT rc; GetClientRect(hwnd, &rc);
        int b = (int)S(6);
        if (p.y < b) return HTTOP;
        if (p.y > rc.bottom - b) return HTBOTTOM;
        if (p.x < b) return HTLEFT;
        if (p.x > rc.right - b) return HTRIGHT;

        // 标题栏右侧三个按钮：必须让点击落到客户端，否则会被当成拖拽
        if (p.y < (int)S(44))
        {
            float bw = S(40), gap = S(4), by = S(8);
            float right = (float)rc.right - S(10);
            for (int i = 0; i < 3; i++)
            {
                float x0 = right - bw * (i + 1) - gap * i;
                if (p.x >= x0 && p.x <= x0 + bw && p.y >= by && p.y <= by + S(28))
                    return HTCLIENT;
            }
            // 提示条里的动作按钮（如「自检」）也在这个高度上，
            // 不放行的话真鼠标点它只会拖动窗口（PostMessage 能点，真鼠标不能 —— 很难发现）
            if (::g.noticeBtnRect.Width > 0 &&
                p.x >= ::g.noticeBtnRect.X && p.x <= ::g.noticeBtnRect.GetRight() &&
                p.y >= ::g.noticeBtnRect.Y && p.y <= ::g.noticeBtnRect.GetBottom())
                return HTCLIENT;
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_ERASEBKGND: return 1;

    case WM_PAINT: Paint(hwnd); return 0;

    case WM_PRINTCLIENT:
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        RenderTo((HDC)wp, rc.right, rc.bottom);
        return 0;
    }

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_DPICHANGED:
    {
        ::g.dpi = (float)HIWORD(wp) / 96.f;
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (::g.dragSlider)
        {
            for (auto& h : ::g.hits)
                if (h.id == ID_SLIDER)
                {
                    float t = (p.x - h.r.X) / h.r.Width;
                    t = std::clamp(t, 0.f, 1.f);
                    ::g.SetActiveVol(t);
                    if (::g.volMode == 0) ::g.ApplyVolumeAll(t);
                    else
                    {
                        auto k = ::g.SelectedKey();
                        for (auto& e : ::g.devVols)
                            if (e.key == k) { ::g.oa.SetSessionVolume(e.instanceId, t); break; }
                    }
                }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        int oldHover = ::g.hover, oldIdx = ::g.hoverIdx;
        ::g.hover = ID_NONE; ::g.hoverIdx = -1;
        for (auto it = ::g.hits.rbegin(); it != ::g.hits.rend(); ++it)
        {
            if (it->r.Contains((float)p.x, (float)p.y))
            {
                ::g.hover = it->id; ::g.hoverIdx = it->index;
                break;
            }
        }
        if (oldHover != ::g.hover || oldIdx != ::g.hoverIdx) InvalidateRect(hwnd, nullptr, FALSE);

        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        ::g.hover = ID_NONE; ::g.hoverIdx = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_NCACTIVATE:
        // 不让系统重画非客户区：否则失焦时 DWM 会给这个 popup 描一圈边框
        return TRUE;

    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE)
        {
            ShowWindow(hwnd, SW_SHOW);
            SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_MOUSEACTIVATE:
        // 正常激活：点击窗口即置顶并获得焦点
        return MA_ACTIVATE;

    case WM_LBUTTONDOWN:
    {
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        for (auto it = ::g.hits.rbegin(); it != ::g.hits.rend(); ++it)
        {
            if (!it->r.Contains((float)p.x, (float)p.y)) continue;
            ::g.pressed = it->id; ::g.pressedIdx = it->index;
            if (it->id == ID_SLIDER)
            {
                // 点一下就跳到该位置：原来只在 WM_MOUSEMOVE 里生效，
                // 单击滑块不会改变数值，用户会以为滑块坏了。
                ::g.dragSlider = true;
                SetCapture(hwnd);
                float t = (p.x - it->r.X) / it->r.Width;
                t = std::clamp(t, 0.f, 1.f);
                ::g.SetActiveVol(t);
                if (::g.volMode == 0) ::g.ApplyVolumeAll(t);
                else if (auto* e = ::g.SelectedEntry())
                {
                    ::g.oa.SetSessionVolume(e->instanceId, t);
                    e->vol = t;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            // 优先级列表：点在复选框列 → 切换勾选；点其它位置 → 手动切换到该设备
            if (it->id == ID_PRIO_LIST && it->index >= 0)
            {
                float cbX = it->r.X + S(24);
                if (p.x >= cbX - S(8) && p.x <= cbX + S(24)) TogglePrioCheck(it->index);
                else SwitchOutputTo(it->index);
            }
            break;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP:
    {
        PerfScope psClick("CLICKUP", 1.0, 0);
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ::g.perfClickAt = GetTickCount64();
        if (::g.dragSlider)
        {
            ::g.dragSlider = false;
            ReleaseCapture();
            SaveSettings();
        }
        bool hitAny = false;
        for (auto it = ::g.hits.rbegin(); it != ::g.hits.rend(); ++it)
        {
            if (!it->r.Contains((float)p.x, (float)p.y)) continue;
            hitAny = true;
            if (it->id == ::g.pressed && it->index == ::g.pressedIdx)
                DoClick(it->id, it->index);
            break;
        }
        // 帮助/设置只通过明确的关闭按钮关闭，避免误点面板外时丢失内容
        ::g.pressed = ID_NONE; ::g.pressedIdx = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &p);
        int delta = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1;

        // 设置面板：主体区域直接滚动（不能被控件命中抢走）
        if (::g.settingsOpen && ::g.settingsBody.Contains((float)p.x, (float)p.y))
        {
            ::g.settingsScroll = std::max(0.f, ::g.settingsScroll + delta * S(36));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        for (auto& h : ::g.hits)
        {
            if (!h.r.Contains((float)p.x, (float)p.y)) continue;
            if (h.id == ID_BT_LIST) ::g.scrollDev = std::max(0, ::g.scrollDev + delta);
            if (h.id == ID_PRIO_LIST) ::g.scrollPrio = std::max(0, ::g.scrollPrio + delta);
            if (h.id == ID_HELP_BODY) ::g.helpScroll = std::max(0.f, ::g.helpScroll + delta * S(36));
            if (h.id == ID_SET_BODY)  ::g.settingsScroll = std::max(0.f, ::g.settingsScroll + delta * S(36));
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
        return 0;
    }

    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU)
        {
            HMENU m = CreatePopupMenu();
            bool connected = LinkUp();
            AppendMenuW(m, MF_STRING, 1, connected ? L"断开" : L"连接");
            AppendMenuW(m, MF_STRING, 2, ::g.phoneMute ? L"取消静音" : L"静音");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 3, L"显示面板");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 4, L"退出");
            POINT p; GetCursorPos(&p);
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
            DestroyMenu(m);
            if (cmd == 1) { if (connected) ::g.bt.Disconnect(); else ConnectSelected(); }
            if (cmd == 2) { ::g.phoneMute = !::g.phoneMute;
                ::g.ApplyMuteAll(::g.phoneMute);
                SaveSettings(); }
            if (cmd == 3) ShowWindowFromTray();
            if (cmd == 4) DestroyWindow(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK)
        {
            ShowWindowFromTray();
        }
        return 0;

    case WM_SHOWME:
        ShowWindowFromTray();
        return 0;

    case WM_CHAR:
        if (::g.pairOpen)
        {
            wchar_t c = (wchar_t)wp;
            std::wstring* target = (::g.pairFocus == 0) ? &::g.pairAddr
                                 : (::g.pairFocus == 1) ? &::g.pairCode : &::g.pairPort;
            size_t limit = (::g.pairFocus == 0) ? 21 : (::g.pairFocus == 1 ? 6 : 5);
            bool ok = (c >= L'0' && c <= L'9') || (::g.pairFocus == 0 && (c == L'.' || c == L':'));
            if (ok && target->size() < limit) *target += c;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (::g.pairOpen)
        {
            std::wstring* target = (::g.pairFocus == 0) ? &::g.pairAddr
                                 : (::g.pairFocus == 1) ? &::g.pairCode : &::g.pairPort;
            size_t limit = (::g.pairFocus == 0) ? 21 : (::g.pairFocus == 1 ? 6 : 5);
            if (wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000))
            {
                // 粘贴：地址/配对码可能来自别处，能贴就不用打
                if (OpenClipboard(hwnd))
                {
                    if (HANDLE hd = GetClipboardData(CF_UNICODETEXT))
                    {
                        if (auto* txt = (const wchar_t*)GlobalLock(hd))
                        {
                            for (const wchar_t* q = txt; *q && target->size() < limit; ++q)
                            {
                                wchar_t c = *q;
                                bool ok = (c >= L'0' && c <= L'9') ||
                                          (::g.pairFocus == 0 && (c == L'.' || c == L':'));
                                if (ok) *target += c;
                            }
                            GlobalUnlock(hd);
                        }
                    }
                    CloseClipboard();
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wp == VK_BACK) { if (!target->empty()) target->pop_back(); }
            else if (wp == VK_TAB) { ::g.pairFocus = (::g.pairFocus + 1) % 3; }
            else if (wp == VK_ESCAPE) { ::g.pairOpen = false; }
            else if (wp == VK_RETURN)
            {
                // 回车 = 直接开始配对
                if (!::g.pairAddr.empty() && !::g.pairCode.empty())
                {
                    std::string a = WideToUtf8(::g.pairAddr);
                    std::string c = WideToUtf8(::g.pairCode);
                    std::string p = WideToUtf8(::g.pairPort);
                    ::g.adb.PairWirelessAsync(a, c, p);
                    ::g.pairStatusLocal = L"正在配对…";
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wp == VK_ESCAPE)
        {
            // Esc 只关浮层，不越权关窗口（否则会绕过「关闭=收进托盘」的设置）
            if (::g.showWizard)
            {
                // 关掉向导 = 用户决定跳过，记下来，否则每次启动都再弹一遍
                ::g.showWizard = false;
                ::g.setupDone = true;
                SaveSettings();
            }
            else if (::g.helpOpen) ::g.helpOpen = false;
            else if (::g.settingsOpen) ::g.settingsOpen = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_TIMER:
        if (wp == 1)
        {
            ::g.bt.Tick();
            // Tick / BootstrapTick / ReconnectTick / KnownWirelessTick / AdaptTick
            // 已全部移到 AdbChannel 的工作线程，避免阻塞 UI
            {
                // 学习到的缓冲 / 已知无线设备 变化时才写盘（避免频繁 IO）
                static std::map<std::string, int> lastBuf;
                static std::set<std::string> lastKnown;
                static std::set<std::string> lastWanted;
                auto cur = ::g.adb.UiBufferPrefs();
                auto kn = ::g.adb.UiKnownWireless();
                auto wd = ::g.adb.UiWantedSerials();
                // 蓝牙历史（改名同步 / 新设备）也在这里落盘，避免每帧写盘
                bool histDirty = ::g.btHistoryDirty;
                ::g.btHistoryDirty = false;

                if (cur != lastBuf || kn != lastKnown || wd != lastWanted || histDirty)
                {
                    lastBuf = cur;
                    lastKnown = kn;
                    lastWanted = wd;
                    SaveSettings();
                }
            }

            // 蓝牙已连接 → 精确匹配对应的 ADB 设备并升级到 ADB 音频
            // 依据：设备自己的蓝牙名（settings secure bluetooth_name）与蓝牙名一致
            if (::g.bt.State() == LinkState::Connected && !::g.bt.DeviceName().empty())
            {
                if (::g.adb.UiDevices().empty()) ::g.adb.Post([]{ ::g.adb.Probe(); });

                // 记住 identity → 蓝牙名，供 ADB 列表显示
                for (auto& d : ::g.adb.UiDevices())
                {
                    if (d.state != "device" || d.btName.empty()) continue;
                    std::wstring dn = Utf8ToWide(d.btName);
                    if (dn == ::g.bt.DeviceName() &&
                        ::g.RememberAdbDevice(d.identity, ::g.bt.DeviceName(), std::string()))
                        SaveSettings();
                }

                // 升级：只对「蓝牙连着的那台」启动 ADB，不再用单一 IP 猜
                // 升级请求交给工作线程，成功后由 ConsumeUpgraded() 取结果
                if (!::g.useAdb && ::g.autoUpgrade && ::g.adb.UiRunningCount() == 0)
                {
                    static int upgradeTick = 0;
                    if (++upgradeTick >= 20)   // 5 秒试一次，避免频繁
                    {
                        upgradeTick = 0;
                        ::g.adb.RequestUpgradeForBt(::g.bt.DeviceName());
                    }
                }
                if (::g.adb.ConsumeUpgraded())
                {
                    ::g.useAdb = true;
                    ::g.bt.Disconnect();       // 释放蓝牙，避免两条链路同时出声
                    ::g.SetNotice(L"已自动升级到 ADB 音频通道", 0, 6000);
                    SaveSettings();
                }
            }

            // 蓝牙模式：周期性刷新设备列表，新配对/移除的设备能自动出现
            if (!::g.useAdb && ::g.bt.State() != LinkState::Connected && ++::g.btScanTick >= 240)  // 60 秒一次（WinRT 枚举较重）
            {
                ::g.btScanTick = 0;
                RefreshDevices();   // 后台枚举，不阻塞 UI
            }
            // 接管外部 scrcpy 不需要 4Hz：只在「自己一个会话都没跑」时才可能用到，
            // 而 FindExternalScrcpy 是全进程快照（7.8ms/次）。降到约 4 秒一次。
            if (::g.useAdb && ++::g.adoptTick >= 16)
            {
                ::g.adoptTick = 0;
                ::g.adb.Post([]{ ::g.adb.AdoptExternal(); });
            }
            // 手动升级的结果：以前失败了完全没有任何反馈，用户只觉得「点了没反应」
            {
                int ur = ::g.adb.UiUpgradeResult();
                if (ur == 1)
                {
                    ::g.adb.ResetUpgradeResult();
                    ::g.adb.ConsumeUpgraded();
                    ::g.useAdb = true;
                    if (::g.pairOpen) ::g.pairStatusLocal = ::g.adb.UiUpgradeStatus();
                    else ::g.SetNotice(L"已升级到 ADB 音频", 0, 4000);
                }
                else if (ur == 2)
                {
                    ::g.adb.ResetUpgradeResult();
                    std::wstring why = ::g.adb.UiUpgradeStatus();
                    if (why.empty()) why = L"升级失败：没找到这台设备的 ADB 连接。";
                    if (::g.pairOpen) ::g.pairStatusLocal = why;
                    else ::g.SetNotice(why, 1, 9000);
                }
            }

            // 配对框开着且地址还空着 → 把 mDNS 查到的地址自动填进去
            if (::g.pairOpen && ::g.pairAddr.empty())
            {
                std::wstring ep = ::g.adb.UiPairingEndpoint();
                if (!ep.empty())
                {
                    ::g.pairAddr = ep;
                    if (::g.pairStatusLocal.find(L"正在查找") != std::wstring::npos)
                        ::g.pairStatusLocal = L"已自动填入手机广播的配对地址，只需再输 6 位配对码。";
                }
                else if (::g.pairStatusLocal.empty())
                {
                    // 找不到就直说，否则用户只会看到一个空输入框
                    ::g.pairStatusLocal = L"没找到手机的配对地址。请确认手机上停在「使用配对码配对设备」界面"
                                          L"（打开「二维码扫描」那个界面时手机不广播，找不到）。也可以手输。";
                }
            }

            // 配对结束后把「正在配对…」换成真实结果（结果文字由工作线程写）
            if (::g.pairOpen && !::g.adb.UiPairBusy() &&
                (::g.pairStatusLocal == L"正在配对…" || ::g.pairStatusLocal == L"正在连接…"))
                ::g.pairStatusLocal.clear();   // 失败时会显示工作线程写的真实原因，按钮回到「重试配对」

            // 每 4 秒把在线 ADB 设备的蓝牙身份（名字+MAC）记进总列表
            {
                static int identTick = 0;
                if (::g.useAdb && ++identTick >= 16)
                {
                    identTick = 0;
                    bool ch = false;
                    for (auto& d : ::g.adb.UiDevices())
                    {
                        if (d.state != "device") continue;
                        if (::g.RememberAdbDevice(d.identity, Utf8ToWide(d.btName), d.btMac)) ch = true;

                        // 老数据迁移：音量/静音以前按 serial 记，同一台设备会有多条。
                        // 看到设备在线就顺手并到 identity 键上（只需做一次）。
                        if (!d.identity.empty() && d.serial != d.identity)
                        {
                            std::wstring oldKey = L"adb:" + Utf8ToWide(d.serial);
                            std::wstring newKey = App::AdbVolKey(d);
                            auto itv = ::g.devVolPref.find(oldKey);
                            if (itv != ::g.devVolPref.end())
                            {
                                if (!::g.devVolPref.count(newKey)) ::g.devVolPref[newKey] = itv->second;
                                ::g.devVolPref.erase(itv);
                                ch = true;
                            }
                            auto itm2 = ::g.devMutePref.find(oldKey);
                            if (itm2 != ::g.devMutePref.end())
                            {
                                if (!::g.devMutePref.count(newKey)) ::g.devMutePref[newKey] = itm2->second;
                                ::g.devMutePref.erase(itm2);
                                ch = true;
                            }
                        }
                    }
                    if (ch) ::g.btHistoryDirty = true;   // 复用同一个落盘标志
                }
            }
            if (::g.useAdb && ++::g.adbTick >= 24)   // 约 6 秒探测一次（运行中列表变化少）
            {
                ::g.adbTick = 0;
                ::g.adb.Post([]{ ::g.adb.Probe(); });   // 运行中也要刷新列表（Probe 内部保护运行状态）
            }

            // 音频会话枚举：每 1 秒一次（原来每 250ms 枚举两遍）
            static int sessTick = 0;
            if (++sessTick >= 4)
            {
                sessTick = 0;
                ::g.devVols.clear();
                for (auto& ss : ::g.oa.AllSessions())
                {
                    App::DevVolEntry e;
                    e.playing = ss.playing;
                    e.vol = (ss.volume >= 0.f) ? ss.volume : 1.f;
                    e.mute = ss.muted;
                    e.instanceId = ss.instanceId;

                    std::string ser = ::g.adb.UiSerialForPid(ss.pid);
                    if (!ser.empty())
                    {
                        e.isBt = false;
                        for (auto& d : ::g.adb.UiDevices())
                            if (d.serial == ser)
                            {
                                e.key = App::AdbVolKey(d);       // identity 稳定，换链路不丢音量
                                e.label = !d.btName.empty() ? Utf8ToWide(d.btName)
                                        : (!d.model.empty() ? Utf8ToWide(d.model) : Utf8ToWide(ser));
                                break;
                            }
                        if (e.key.empty()) e.key = L"adb:" + Utf8ToWide(ser);   // 设备已消失，退回 serial
                    }
                    else if (ss.isBt)
                    {
                        // 显示名形如 "… (示例手机 A2DP SNK)"
                        std::wstring nm = ss.displayName;
                        size_t p1 = nm.find(L'('), p2 = nm.rfind(L" A2DP");
                        if (p1 != std::wstring::npos && p2 != std::wstring::npos && p2 > p1)
                            nm = nm.substr(p1 + 1, p2 - p1 - 1);
                        std::string bmac;
                        for (auto& bd : ::g.btDevs) if (bd.name == nm) { bmac = bd.mac; break; }
                        e.key = App::BtVolKey(nm, bmac);
                        e.isBt = true;
                        e.label = nm;
                    }
                    else continue;

                    ::g.devVols.push_back(e);
                }

                // phoneSessions 由 ADB 条目派生，不再单独枚举一遍
                ::g.phoneSessions.clear();
                for (auto& e : ::g.devVols)
                {
                    if (e.isBt) continue;
                    PhoneSessionInfo ps;
                    ps.found = true; ps.playing = e.playing;
                    ps.volume = e.vol; ps.muted = e.mute;
                    ps.instanceId = e.instanceId;
                    ::g.phoneSessions.push_back(ps);
                }

                if (!::g.phoneSessions.empty()) ::g.session = ::g.phoneSessions.front();
                else if (!::g.devVols.empty())
                {
                    auto& e0 = ::g.devVols.front();
                    ::g.session = PhoneSessionInfo{};
                    ::g.session.found = true; ::g.session.playing = e0.playing;
                    ::g.session.volume = e0.vol; ::g.session.muted = e0.mute;
                    ::g.session.instanceId = e0.instanceId;
                }
                else ::g.session = PhoneSessionInfo{};

                // 汇总：任一设备确实在出声（峰值表，不用 dumpsys）
                int anyPlaying = 0;
                for (auto& e : ::g.devVols) if (e.playing) anyPlaying = 1;
                ::g.phonePlaying = anyPlaying;
            }


            // 默认输出设备：每 1 秒刷一次（原来每 250ms）
            static int defTick = 0;
            if (++defTick >= 4) { defTick = 0; RefreshCurrentDefault(); }

            // ---- 缓冲变更：等空闲时执行 ----
            // 重启 scrcpy 有让 Android 不迁移音频流的风险，所以只在「没在播放」时做。
            // 这既避免打断音乐，也保证下次播放一开始就是对的缓冲。
            for (auto& ser : ::g.adb.UiRunningSerials())
            {
                if (!::g.adb.UiHasPendingBuffer(ser)) continue;

                bool playing = false;
                for (auto& e : ::g.devVols)
                    if (e.key == (L"adb:" + Utf8ToWide(ser))) { playing = e.playing; break; }

                // 开启「空闲时再调整」→ 等没在播放时才重启；关闭 → 立即执行（可能打断）
                if (!::g.idleReset || !playing)
                {
                    std::string s2 = ser;
                    ::g.adb.Post([s2]{ ::g.adb.ApplyPendingBuffer(s2); });
                }
            }

            // 预设模式下的「空闲重置」：不开自适应时，也定期在空闲时重建通道，
            // 把可能已经退化的音频通道提前刷干净，避免播放中途出问题。
            if (::g.idleReset && !::g.adb.AutoAdapt())
            {
                static DWORD lastIdleReset = 0;
                DWORD now2 = GetTickCount();
                if (lastIdleReset == 0 || now2 - lastIdleReset > 20 * 60 * 1000)   // 20 分钟一次
                {
                    for (auto& ser : ::g.adb.UiRunningSerials())
                    {
                        bool playing = false;
                        for (auto& e : ::g.devVols)
                            if (e.key == (L"adb:" + Utf8ToWide(ser))) { playing = e.playing; break; }
                        if (!playing)
                        {
                            lastIdleReset = now2;
                            std::string s2 = ser;
                            ::g.adb.Post([s2]{ ::g.adb.ApplyIdleReset(s2); });
                            break;
                        }
                    }
                }
            }

            PollBtScan();   // 取回后台枚举的蓝牙设备列表

            // 工作线程请求断开蓝牙 → 在这里（UI 线程）真正执行
            if (::g.btDisconnectRequest.exchange(false))
            {
                ::g.bt.Disconnect();
                ::g.SetNotice(L"音频被蓝牙占用，已自动断开蓝牙链路", 0, 10000);
            }

            // ---- ADB 模式下自动转发「所有」在线设备 ----
            // 不需要用户逐个点 ▶：只要设备已授权且在线，就自动开始转发。
            // 用户手动点过 ■ 的设备会记在 userStopped 里，不再自动拉起。
            if (::g.useAdb)
            {
                // 迁移：旧版本把勾选存成 IP，这里改成存设备身份，IP 变了也不会丢
                static bool migrated = false;
                if (!migrated && !::g.adb.UiDevices().empty())
                {
                    migrated = true;
                    auto devs = ::g.adb.UiDevices();
                    bool changed = false;
                    for (auto& d : devs)
                    {
                        if (d.identity.empty()) continue;
                        if (::g.autoDevices.count(d.serial) && !::g.autoDevices.count(d.identity))
                        {
                            ::g.autoDevices.erase(d.serial);
                            ::g.autoDevices.insert(d.identity);
                            changed = true;
                        }
                    }
                    if (changed) SaveSettings();
                }

                // 用去重后的列表：否则同一台设备的每个连接入口都会开一个 scrcpy
                // （手机同时开 tcpip 5555 + 无线调试时实测会开 4 个）
                auto merged = ::g.adb.UiUniqueDevices();
                for (auto& d : merged)
                {
                    if (d.state != "device" || d.serial.empty()) continue;
                    if (::g.adb.UiIsRunning(d.serial)) continue;
                    // 按设备身份判断（IP 变化不影响勾选状态）；兼容旧版按 serial 的记录
                    bool checked = (!d.identity.empty() && ::g.autoDevices.count(d.identity))
                                || ::g.autoDevices.count(d.serial);
                    if (!checked) continue;                           // 只自动连勾选的

                    // 「用户停过」要按身份判断：停的是 A 端口，不代表可以自动起 B 端口
                    bool stopped = ::g.userStopped.count(d.serial) > 0;
                    if (!stopped && !d.identity.empty())
                        for (auto& raw : ::g.adb.UiDevices())
                            if (raw.identity == d.identity && ::g.userStopped.count(raw.serial)) { stopped = true; break; }
                    if (stopped) continue;

                    // 「自动重连」关掉后：只有启动阶段才自动起会话。
                    // 过了启动阶段就完全不碰 —— 开关一次手机 WiFi 即可断开，
                    // 且不会自己连回来（要恢复就点「刷新」）。
                    if (!::g.adb.AutoConnectAllowed()) continue;

                    std::string s2 = d.serial;
                    ::g.adb.Post([s2]{ ::g.adb.StartDevice(s2); });
                }
            }

            // ---- 周期性检查：媒体音频是否被蓝牙抢走 ----
            // 这是「手机在播但电脑静音」最常见的真正原因：
            // 手机蓝牙还连着电脑，Android 优先把媒体音频送蓝牙，
            // 而程序升级到 ADB 后已断开蓝牙链路 → 音乐被送进黑洞。
            // 只要发现这种情况就断开蓝牙，让音频回到可被 ADB 抓取的路径。
            {
                static DWORD lastBtCheck = 0;
                DWORD nowB = GetTickCount();
                if (::g.useAdb && (lastBtCheck == 0 || nowB - lastBtCheck > 30000))   // 30 秒一次
                {
                    auto running = ::g.adb.UiRunningSerials();
                    if (!running.empty())
                    {
                        lastBtCheck = nowB;
                        std::string s2 = running.front();
                        ::g.adb.Post([s2]{
                            if (::g.adb.IsMediaOnBluetooth(s2))
                            {
                                ::g.btDisconnectRequest = true;   // 交给 UI 线程执行
                                ::g.SetNotice(L"音频被蓝牙占用，正在断开蓝牙链路…", 0, 10000);
                            }
                        });
                    }
                }
            }

            // ---- 静音自愈 ----
            // 场景：手机媒体在播（PLAYING），但电脑端一点声音都没有。
            // 原因是 scrcpy 新建 AudioPolicyMix 时，已在播的 AudioTrack 不会被迁移进去；
            // 只发 dispatch play 是空操作，必须「暂停→播放」强制 App 重建 AudioTrack。
            for (auto& ser : ::g.adb.UiRunningSerials())
            {
                auto& st = ::g.healState[ser];
                DWORD spawn = ::g.adb.UiSpawnMs(ser);
                if (st.spawnMs != spawn)
                {
                    st.spawnMs = spawn;
                    st.silentSince = 0;
                    st.lastNudge = 0;
                    st.diagDone = false;
                }

                bool playing = false;
                for (auto& e : ::g.devVols)
                    if (e.key == (L"adb:" + Utf8ToWide(ser))) { playing = e.playing; break; }

                // 持续监控：只要「电脑端完全没声音」，就盯着；持续 15 秒就查一次手机状态。
                // 播放中途被抢走音频（比如游戏抢焦点把音乐切到听筒）也能抓到，
                // 而不是只在会话刚启动时查一次。
                DWORD nowH = GetTickCount();
                if (playing)
                {
                    st.silentSince = 0;
                    st.diagDone = false;
                }
                else
                {
                    if (st.silentSince == 0) st.silentSince = nowH;

                    bool longEnough = (nowH - st.silentSince > 15000);
                    bool notThrottled = (st.lastNudge == 0 || nowH - st.lastNudge > 60000);
                    if (longEnough && notThrottled)
                    {
                        st.lastNudge = nowH;
                        bool firstDiag = !st.diagDone;
                        st.diagDone = true;

                        std::string s2 = ser;
                        int vol = 0; bool mute = false;
                        for (auto& e : ::g.devVols)
                            if (e.key == (L"adb:" + Utf8ToWide(ser)))
                            { vol = (int)(e.vol * 100); mute = e.mute; break; }
                        std::wstring extra = L"pc_volume=" + std::to_wstring(vol)
                                           + L"% pc_mute=" + std::to_wstring((int)mute)
                                           + L" silent_for=" + std::to_wstring(nowH - st.silentSince) + L"ms";

                        // 自己把音量调到很低或静音了 → 这不是故障，别去打断播放
                        if (mute || vol < 5) continue;

                        ::g.adb.Post([s2, extra, firstDiag]{
                            if (!::g.adb.IsMediaPlaying(s2)) return;   // 手机没在播就不管
                            if (firstDiag) ::g.adb.DumpSilentDiag(s2, extra);
                            if (::g.adb.IsMediaOnBluetooth(s2))
                            {
                                ::g.btDisconnectRequest = true;
                                ::g.SetNotice(L"音频被蓝牙占用，正在断开蓝牙链路…", 0, 10000);
                                return;
                            }
                            ::g.SetNotice(L"检测到没有声音，正在尝试恢复…", 1, 10000);
                            ::g.adb.NudgeMedia(s2);                    // 强制重建音频流
                        });
                    }
                }
            }
            for (auto it = ::g.healState.begin(); it != ::g.healState.end();)
            {
                if (!::g.adb.UiIsRunning(it->first)) it = ::g.healState.erase(it);
                else ++it;
            }

            if (!::g.volSelInit && ::g.volMode == 1 && !::g.devVols.empty() && !::g.SelectedEntry())
            {
                auto rows = CurrentRows();
                for (int i = 0; i < (int)rows.size(); i++)
                {
                    std::wstring key = ::g.useAdb ? (L"adb:" + Utf8ToWide(rows[i].serial))
                                                  : App::BtVolKey(rows[i].name, rows[i].btMac);
                    if (::g.FindDevVol(key))
                    {
                        if (::g.useAdb) ::g.adbSel = i; else ::g.btSel = i;
                        ::g.volSelInit = true;   // 真正选中了才算完成
                        break;
                    }
                }
            }


            // 逐设备同步音量/静音；系统全局音量绝不触碰
            //   全局模式：所有设备跟随同一个滑块值
            //   单个模式：每台设备只跟随它自己的设定，未设定过的不动
            for (auto& e : ::g.devVols)
            {
                float want;
                bool wmute;
                if (::g.volMode == 0)
                {
                    want = ::g.phoneVol;
                    wmute = ::g.phoneMute;
                    // 只在这台设备还没自己的基准时写入，之后不再每秒覆写。
                    // 否则「单个模式」下调好的值会被全局模式冲掉（切回去就变回原来的）。
                    if (!::g.devVolPref.count(e.key))  ::g.devVolPref[e.key]  = want;
                    if (!::g.devMutePref.count(e.key)) ::g.devMutePref[e.key] = wmute;
                }
                else
                {
                    auto it = ::g.devVolPref.find(e.key);
                    auto itm = ::g.devMutePref.find(e.key);
                    if (it == ::g.devVolPref.end() && itm == ::g.devMutePref.end()) continue;
                    want = (it != ::g.devVolPref.end()) ? it->second : e.vol;
                    wmute = (itm != ::g.devMutePref.end()) ? itm->second : e.mute;
                }

                if (!::g.dragSlider && std::abs(e.vol - want) > 0.02f)
                    { PerfScope pV("  SetSessionVolume", 20.0, 0); ::g.oa.SetSessionVolume(e.instanceId, want); }
                if (e.mute != wmute)
                    { PerfScope pM("  SetSessionMute", 20.0, 0); ::g.oa.SetSessionMute(e.instanceId, wmute); }
            }

            bool linkUp = LinkUp();
            if (linkUp && ::g.fixedMode && !::g.routed) ApplyRouting();
            if (!linkUp && ::g.routed) RestoreRouting();

            UpdateTray();

            // 内容没变化就不重绘（拖滑块时需要连续重绘）
            static size_t lastSig = 0;
            size_t sig = ::g.VisualSig();
            if (::g.dragSlider || sig != lastSig)
            {
                lastSig = sig;
                if (!IsIconic(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_ENDSESSION:
        // 注销 / 关机：Windows 不保证再走 WM_DESTROY，这里不还原就会把系统默认输出
        // 留在「手机那条已经不存在的线」上 —— 下次开机整机没声音，用户只会去修电脑。
        if (wp) RestoreRouting();
        break;

    case WM_DESTROY:
        // 退出前必须收尾：先停工作线程，再杀 scrcpy 子进程（否则会变孤儿继续出声）。
        // 收尾单独包 try：StopWorker 里的 thread::join 可能抛 std::system_error。
        // 如果让异常跑到外层包装的 catch，PostQuitMessage 就被跳过了 ——
        // 窗口已销毁、托盘已删、进程却永远收不到 WM_QUIT，用户只能去任务管理器。
        try
        {
            ::g.adb.StopWorker();
            ::g.adb.Shutdown();
            if (::g.trayAdded) { Shell_NotifyIconW(NIM_DELETE, &::g.nid); ::g.trayAdded = false; }
        }
        catch (const std::exception& e) { PerfLog("EXCEPTION WM_DESTROY: %s", e.what()); }
        catch (...) { PerfLog("EXCEPTION WM_DESTROY: unknown"); }
        PostQuitMessage(0);          // 无条件执行，否则进程挂死
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 异常边界：Win32 回调里漏出去的 C++ 异常会直接终止进程，而且什么都不留。
// 这里兜住、记一行、让程序继续跑（GDI+ 分配失败、容器越界都走这里）。
//
// 注意：这是「包装函数」而不是「在 WndProcInner 里套 try」——
// 后者需要把 850 行 switch 整体缩进一级，diff 会变成整文件重写，review 时看不清。
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    try { return WndProcInner(hwnd, msg, wp, lp); }
    catch (const std::exception& e) { PerfLog("EXCEPTION WndProc msg=0x%04X: %s", msg, e.what()); }
    catch (...) { PerfLog("EXCEPTION WndProc msg=0x%04X: unknown", msg); }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------ 入口

// 诊断日志每次启动重建。放在这里统一做一次：
// PerfLog 定义在头文件里，每个编译单元各有一份静态变量，
// 如果在 PerfLog 内部截断，每个 TU 的首次调用都会清一次，互相把日志抹掉。
static void ResetPerfLogOnce()
{
    wchar_t ap[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"APPDATA", ap, MAX_PATH) == 0) return;
    std::wstring d = std::wstring(ap) + L"\\WaveTether";
    CreateDirectoryW(d.c_str(), nullptr);
    std::wstring f = d + L"\\perf.log";
    FILE* t = nullptr;
    _wfopen_s(&t, f.c_str(), L"w");
    if (t) fclose(t);
}

// ------------------------------------------------------------------ 命令行模式
//
// --version   打印版本号后退出
// --selftest  无界面跑一遍组件检查，用退出码报告结果（0 全通过 / 1 有问题）
// --help      打印用法
//
// 为什么要它：以前「发布前冒烟」只能人眼开窗口点一遍，没法自动化，
// 也没法在 CI 里跑。这三个开关把「能不能启动」变成一个退出码。

// CLI 输出统一走这里，不用 stdio。
//
// 原因：GUI 子系统程序的 CRT stdout 默认没接到继承来的句柄上，
// 用 fwprintf 在「管道重定向」场景下什么都收不到（实测 CI 里就是空的）。
// 直接 WriteConsoleW / WriteFile 反而两种场景都对：
//   · 控制台：写宽字符
//   · 管道/文件：转 UTF-8 写字节
static void CliOut(const wchar_t* fmt, ...)
{
    wchar_t buf[2048];
    va_list a;
    va_start(a, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, a);
    va_end(a);

    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return;

    DWORD n = 0;
    if (GetFileType(h) == FILE_TYPE_CHAR)
        WriteConsoleW(h, buf, (DWORD)wcslen(buf), &n, nullptr);
    else
    {
        std::string u = PerfU8(buf);
        WriteFile(h, u.data(), (DWORD)u.size(), &n, nullptr);
    }
}

static bool g_cliOwnConsole = false;

static int RunSelfTest(const std::wstring& exeDir)
{
    int bad = 0;
    // ok=false 计失败；skip=true 只提示不计（例如蓝牙版本来就没有 ADB）
    auto line = [&bad](bool ok, const wchar_t* what, const std::wstring& detail, bool skip = false)
    {
        if (!ok && !skip) ++bad;
        std::wstring d = detail;
        for (auto& c : d) if (c == L'\n' || c == L'\r') c = L' ';
        CliOut(L"[%s] %-14s %s\n", skip ? L" -- " : (ok ? L" OK " : L"FAIL"), what, d.c_str());
    };

    ::g.adb.Init(exeDir, /*probeOnly=*/true);

    line(true, L"版本", std::wstring(APP_VERSION) + L" (" + APP_VERSION_FULL + L")");

    // 形态看「exe 里有没有内嵌资源」，不看临时目录里释放没释放：
    // 干净机器上还没正常启动过，按释放结果判会把单文件版误报成便携版/蓝牙版。
    bool embedded = ::g.adb.HasEmbedded();
    bool pending = ::g.adb.EmbeddedPending();
    bool hasAdb = ::g.adb.HasAdb();
    // 按「旁边有没有 tools 目录」判形态，不按「有没有找到 adb」——
    // 后者可能来自 PATH，会把「便携版但 tools 缺失」误报成正常便携版。
    std::wstring toolsDir = exeDir + L"\\tools";
    bool hasToolsDir = GetFileAttributesW(toolsDir.c_str()) != INVALID_FILE_ATTRIBUTES;
    line(true, L"版本形态", embedded
              ? (pending ? std::wstring(L"单文件版（内嵌工具，首次正常启动时释放）")
                         : (::g.adb.EmbeddedRoot().empty()
                                ? std::wstring(L"单文件版（内嵌工具，当前未释放）")
                                : std::wstring(L"单文件版（已释放到临时目录）")))
              : (hasToolsDir ? std::wstring(L"便携版（exe 旁边的 tools）")
                             : std::wstring(L"蓝牙版（无 ADB 组件）")));

    if (!::g.adb.EmbedError().empty())
        line(false, L"内嵌释放", ::g.adb.EmbedError());
    else if (!::g.adb.EmbeddedStale().empty())
        // 目录在但不完整：正常启动会自动重建，所以不算失败，但不能说成「尚未写盘」
        line(true, L"内嵌释放", L"运行时目录已存在但不完整（" + ::g.adb.EmbeddedStale() +
                                L"），下次正常启动会自动重新释放", true);
    else if (pending)
        line(true, L"内嵌释放", L"内嵌资源完整，尚未写盘（自检不落盘；首次正常启动会释放到 %TEMP%）", true);
    else if (embedded)
        line(true, L"内嵌释放", ::g.adb.EmbeddedRoot());

    // 蓝牙版本来就没有 ADB 组件 —— 那是设计如此，不是故障。
    bool btOnly = !hasAdb && !embedded && !hasToolsDir;
    if (pending)
    {
        line(true, L"adb", L"内嵌清单已校验；首次正常启动释放后可用", true);
        line(true, L"scrcpy", L"同上", true);
        line(true, L"scrcpy-server", L"同上", true);
    }
    else
    {
        line(hasAdb, L"adb", hasAdb ? ::g.adb.AdbExePath()
                                    : std::wstring(L"未找到（ADB 通道不可用，蓝牙不受影响）"), btOnly);
        if (hasAdb)
        {
            line(::g.adb.HasScrcpy(), L"scrcpy", ::g.adb.HasScrcpy() ? std::wstring(L"已找到") : std::wstring(L"缺失"));
            line(::g.adb.HasServer(), L"scrcpy-server", ::g.adb.HasServer() ? std::wstring(L"已找到") : std::wstring(L"缺失"));
            // 只判「文件在」不够：0 字节 / 被截断的 adb.exe 同样存在，
            // 用户拿到的是「点了没反应」，自检却报全部通过。
            std::wstring why = ::g.adb.VerifyTools();
            line(why.empty(), L"工具可用性",
                 why.empty() ? std::wstring(L"adb / scrcpy / 依赖 DLL 结构校验通过") : why);
        }
    }

    // 配置目录可写（写不进去等于每次启动都丢设置）
    {
        wchar_t ap[MAX_PATH]{};
        bool ok = GetEnvironmentVariableW(L"APPDATA", ap, MAX_PATH) > 0;
        std::wstring dir = ok ? (std::wstring(ap) + L"\\WaveTether") : std::wstring();
        if (ok) CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring probe = dir + L"\\.writetest";
        FILE* f = nullptr;
        _wfopen_s(&f, probe.c_str(), L"w");
        ok = ok && f != nullptr;
        if (f) fclose(f);
        DeleteFileW(probe.c_str());
        line(ok, L"配置目录", ok ? dir : std::wstring(L"%APPDATA%\\WaveTether 不可写"));
    }

    CliOut(L"\n结果：%s\n", bad == 0 ? L"全部通过" : L"有项目未通过");
    return bad == 0 ? 0 : 1;
}

// 双击启动时，控制台是本进程自己开的，退出就消失。
// 停一下等按键，否则用户只会看到窗口闪一下。
static void CliPauseIfOwnConsole()
{
    if (!g_cliOwnConsole) return;
    CliOut(L"\n按任意键关闭…");
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (in && in != INVALID_HANDLE_VALUE)
    {
        INPUT_RECORD rec;
        DWORD n = 0;
        while (ReadConsoleInputW(in, &rec, 1, &n) && n == 1)
            if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) break;
    }
}

// 返回 true 表示「已处理完，直接退出」
static bool HandleCli(const wchar_t* cmdLine, int& exitCode)
{
    std::wstring cl = cmdLine ? cmdLine : L"";
    bool wantVersion = cl.find(L"--version") != std::wstring::npos;
    bool wantSelfTest = cl.find(L"--selftest") != std::wstring::npos;
    bool wantHelp = cl.find(L"--help") != std::wstring::npos;
    if (!wantVersion && !wantSelfTest && !wantHelp) return false;

    // 双击启动（既没父控制台、也没重定向）时自己开一个控制台，
    // 否则用户点了 --selftest 屏幕上什么都不会出现。
    {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD t = h ? GetFileType(h) : FILE_TYPE_UNKNOWN;
        bool usable = h && h != INVALID_HANDLE_VALUE &&
                      (t == FILE_TYPE_CHAR || t == FILE_TYPE_PIPE || t == FILE_TYPE_DISK);
        if (!usable && AllocConsole())
        {
            SetStdHandle(STD_OUTPUT_HANDLE,
                         CreateFileW(L"CONOUT$", GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, 0, nullptr));
            g_cliOwnConsole = true;   // 双击启动：输出完要停一下，否则窗口一闪就没
        }
    }

    if (wantHelp)
    {
        CliOut(L"WaveTether %s\n\n"
               L"  WaveTether.exe                正常启动（无参数）\n"
               L"  WaveTether.exe --version      打印版本号\n"
               L"  WaveTether.exe --selftest     无界面自检，退出码 0=全部通过 1=有问题\n"
               L"  WaveTether.exe --help         显示这段说明\n",
               APP_VERSION);
        exitCode = 0;
        CliPauseIfOwnConsole();
        return true;
    }

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring appDir = exePath;
    { size_t slash = appDir.find_last_of((wchar_t)0x5C); if (slash != std::wstring::npos) appDir = appDir.substr(0, slash); }

    if (wantSelfTest)
    {
        exitCode = RunSelfTest(appDir);
        CliPauseIfOwnConsole();
        return true;
    }

    CliOut(L"WaveTether %s (%s)\n", APP_VERSION, APP_VERSION_FULL);
    exitCode = 0;
    CliPauseIfOwnConsole();
    return true;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int)
{
    // 命令行模式必须在单实例检查之前：否则程序已经在跑时，
    // WaveTether.exe --version 只会去唤醒已有实例，一个字都不打印。
    {
        int code = 0;
        if (HandleCli(cmdLine, code)) return code;
    }

    // 单实例
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, kSingleInstanceEvent);
    bool isNew = (ev && GetLastError() != ERROR_ALREADY_EXISTS);
    if (!isNew)
    {
        if (ev) { SetEvent(ev); CloseHandle(ev); }
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ResetPerfLogOnce();          // 日志每次启动重建（必须早于任何 PerfLog 调用）

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring appDir = exePath;
    { size_t slash = appDir.find_last_of((wchar_t)0x5C); if (slash != std::wstring::npos) appDir = appDir.substr(0, slash); }

    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdiToken, &gsi, nullptr);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    LoadSettings();
    ApplyAutoStart();
    ::g.bt.Init();
    ::g.oa.Init();
    ::g.adb.Init(appDir);
    // 首次使用：显示设置向导（已有设备在转发说明早就配好了）
    if (!::g.setupDone && ::g.autoDevices.empty()) ::g.showWizard = true;

    ::g.adb.StartWorker();        // 所有 adb IO 在工作线程
    ::g.adb.Post([]{ ::g.adb.AdoptExternal(); });
    // ADB 模式下不启动就枚举蓝牙：那一套会把 Windows.Devices.Enumeration /
    // Windows.Media.Devices / Windows.UI / icu 等 13 个模块拉进来，
    // 实测白扁 8.1MB 工作集 + 6 个线程。切到蓝牙标签页时会再枚举（ID_TAB_BT）。
    if (!::g.useAdb) RefreshDevices();
    RefreshOutputs();
    RefreshCurrentDefault();
    RecoverRoutingAfterCrash();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    wc.hIconSm = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    RegisterClassExW(&wc);

    // 先用主显示器的 DPI 定初始尺寸：窗口创建后才拿到 GetDpiForWindow 的话，
    // 高 DPI 下会先按 1.0 排版再跳一次，肉眼能看到闪一下
    {
        UINT dpi = GetDpiForSystem();
        ::g.dpi = (dpi > 0) ? (float)dpi / 96.f : 1.f;
    }
    int w = (int)(960 * ::g.dpi), h = (int)(556 * ::g.dpi);
    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"WaveTether",
                             WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 0;

    SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW(hInst, MAKEINTRESOURCEW(101)));
    SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)LoadIconW(hInst, MAKEINTRESOURCEW(101)));

    // Win11 圆角
    DWORD pref = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(g_hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &pref, sizeof(pref));
    // 去掉系统画的窗口边框：WS_THICKFRAME 的窗口在失焦时会被 DWM 描一圈白边
    {
        COLORREF none = 0xFFFFFFFE;   // DWMWA_COLOR_NONE
        DwmSetWindowAttribute(g_hwnd, 34 /*DWMWA_BORDER_COLOR*/, &none, sizeof(none));
        // 标题栏也跟随（虽然我们自己画，但边框会露出来）
        COLORREF cap = 0x00FFFFFF & ::g.th.Bg;
        DwmSetWindowAttribute(g_hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof(cap));
    }

    ::g.dpi = (float)GetDpiForWindow(g_hwnd) / 96.f;

    // 托盘
    ::g.nid.cbSize = sizeof(::g.nid);
    ::g.nid.hWnd = g_hwnd;
    ::g.nid.uID = TRAY_ID;
    ::g.nid.uCallbackMessage = WM_TRAY;
    ::g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    ::g.nid.hIcon = MakeTrayIcon(false);
    lstrcpynW(::g.nid.szTip, L"WaveTether", 128);
    ::g.trayAdded = Shell_NotifyIconW(NIM_ADD, &::g.nid) != FALSE;

    // --tray：静默启动（开机自启用）
    bool startHidden = (wcsstr(GetCommandLineW(), L"--tray") != nullptr);
    if (!startHidden) ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    SetTimer(g_hwnd, 1, 250, nullptr);

    // 第二次启动时唤起本窗口（命名事件）
    {
        HANDLE ev2 = OpenEventW(SYNCHRONIZE, FALSE, kSingleInstanceEvent);
        if (ev2)
        {
            static HANDLE evStatic = ev2;
            static HWND hwndStatic = g_hwnd;
            CreateThread(nullptr, 0, [](LPVOID) -> DWORD
            {
                for (;;)
                {
                    if (WaitForSingleObject(evStatic, INFINITE) != WAIT_OBJECT_0) return 0;
                    PostMessageW(hwndStatic, WM_SHOWME, 0, 0);
                }
            }, nullptr, 0, nullptr);
        }
    }

    // 自动连接上次设备
    // 只有「还没配过 ADB 设备」时才自动连蓝牙。
    // 已经勾选了 ADB 设备的情况下，蓝牙只会在首次引导时用到，
    // 自动连上反而会被「音频被蓝牙占用」的检查立刻断开，白折腾一次。
    if (::g.autoConnect && !::g.lastDeviceId.empty() && ::g.autoDevices.empty())
        ::g.bt.Connect(::g.lastDeviceId, ::g.lastDeviceName);

    // 临时诊断：记录启动状态，方便把日志和复现步骤对上
    {
        RECT wr{}; GetWindowRect(g_hwnd, &wr);
        PerfLog("==== STARTUP dpi=%.2f window=%dx%d useAdb=%d devices=%zu ====",
                ::g.dpi, (int)(wr.right - wr.left), (int)(wr.bottom - wr.top),
                (int)::g.useAdb, ::g.adb.UiDevices().size());
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RestoreRouting();   // 退出时还原系统默认播放设备
    GdiplusShutdown(g_gdiToken);
    CoUninitialize();
    return 0;
}
