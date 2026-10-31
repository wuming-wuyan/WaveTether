// ADB 音频通道：通过 adb + scrcpy 把手机声音转发到电脑
// 有线优先 → 无线兜底 → 都失败降级蓝牙
#pragma once
#include <string>
#include <windows.h>
#include "perf.h"
#include <vector>
#include <map>
#include <set>
#include <utility>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <functional>
#include <memory>

enum class AdbState { Idle, Probing, Wired, Wireless, Running, Failed };

struct AdbDevice
{
    std::string serial;     // adb 传输地址："001122334455" 或 "192.168.1.5:5555"
    std::string state;      // "device" / "unauthorized" / "offline"
    bool isWired = false;   // serial 不含 ':' 即视为有线
    std::string identity;   // 设备真实身份（ro.serialno），用于合并 USB/无线
    std::string model;      // ro.product.model
    std::string btName;     // 设备自己的蓝牙名（settings secure bluetooth_name）
    std::string btMac;      // 设备自己的蓝牙 MAC（12 位大写十六进制，可能为空：部分 ROM 限制）
    // 无线调试配对状态：0=不知道 1=已配对（keystore 里有电脑的公钥，重启不掉）
    // 2=未配对（只有 tcpip 5555，重启后必须插线）。
    // 以前只看「serial 是不是 mDNS 名字」，那是瞬时信号：adb server 重启、
    // TTL 过期都会让它短暂消失，于是明明配过对也显示成「未配对」。
    int pairedState = 0;
};

// scrcpy stderr 里的丢样本计数（缓冲溢出 = 卡顿）
struct DropBox
{
    std::atomic<int> drops{ 0 };
    int bufMs = 0;
};

// 单台设备的转发会话（多设备同时播放 = 多个会话并存）
struct AdbSession
{
    std::string serial;        // adb 传输地址
    void* proc = nullptr;      // PROCESS_INFORMATION*
    bool running = false;
};

class AdbChannel
{
public:
    std::string RawAdb(const std::string& args, int timeoutMs = 8000) { return RunAdb(args, timeoutMs); }

    // ---- 后台工作线程：所有 adb IO（100~1500ms）都放到这里，不阻塞 UI ----
    void StartWorker();
    void StopWorker();
    void Post(std::function<void()> fn);
    // BT 升级到 ADB 的异步结果（工作线程置位，UI 取走）
    bool ConsumeUpgraded() { return upgradedToAdb_.exchange(false); }
    void RequestUpgradeForBt(const std::wstring& btName, const std::string& btMac = std::string());
    // 手动升级的结果：0=无 1=成功 2=失败。UI 用它给反馈（以前失败是完全静默的）
    int UiUpgradeResult() const { return upgradeResult_; }
    std::wstring UiUpgradeStatus() const
    {
        std::lock_guard<std::mutex> lk(pairMtx_);
        return upgradeStatus_;
    }
    void ResetUpgradeResult() { upgradeResult_ = 0; }
    // 已经配对过的设备只需要 adb connect，不需要配对码。
    // mDNS 失效、端口变了的时候，这是唯一还能用的手动入口。
    void ConnectDirectAsync(const std::string& addr);

    // Android 11+ 无线调试配对（adb pair）。
    // 配对端口和连接端口都是随机的，由手机显示；配对成功后凭据持久化在手机上，
    // 之后手机重启也不用再插线 —— 这是 tcpip 5555 做不到的。
    // 全过程在后台线程执行，结果用 UiPairStatus() 轮询。
    void PairWirelessAsync(const std::string& addr, const std::string& code, const std::string& portHint);
    void CancelPairing() { pairCancel_ = true; }
    // 从 mDNS 找「配对码界面」广播的 _adb-tls-pairing 服务，拿到 ip:port。
    // 手机打开「使用配对码配对设备」时才有；开「二维码扫描」时没有（实测确认）。
    void RefreshPairingEndpointAsync();
    std::wstring UiPairingEndpoint() const
    {
        std::lock_guard<std::mutex> lk(pairMtx_);
        return pairEndpoint_;
    }
    std::wstring UiPairStatus() const
    {
        std::lock_guard<std::mutex> lk(pairMtx_);
        return pairStatus_;
    }
    bool UiPairBusy() const { return pairBusy_; }
    // 配对结果：0=还没试过 1=成功 2=失败。UI 用它决定按钮是「配对」还是「完成」。
    int UiPairResult() const { return pairResult_; }
    void ResetPairResult() { pairResult_ = 0; }
    // 上一轮「全部已知无线 IP 都连不上」= 手机上的 ADB 端口根本没开。
    // 这是最常见的情况（手机重启后 tcpip 5555 失效），必须能告诉用户，
    // 否则界面只会一直显示「未找到设备」，用户无从判断。
    bool UiWirelessAllFailed() const
    {
        int t = lastRoundTry_.load();
        return t > 0 && lastRoundFail_.load() == t;
    }


    // 随包组件是否齐全（adb / scrcpy）。缺失时界面要明确告诉用户，
    // 否则「只拷了 exe」这种最常见的情况会表现为「什么都不工作」且无任何提示。
    bool HasAdb() const { return !adbPath_.empty(); }
    // 只给「自检/命令行」这种非热路径读一下路径用（启动后不变，不用加锁）
    std::wstring AdbExePath() const { return adbPath_; }
    bool HasScrcpy() const { return !scrcpyPath_.empty(); }
    bool HasServer() const;
    // 单文件版释放出来的运行时目录（空=分离版，不写临时文件）
    const std::wstring& EmbeddedRoot() const { return embeddedRoot_; }  // 单文件版释放出来的工具根目录
    const std::wstring& EmbedError() const { return embedError_; }       // 单文件版释放失败原因（空=正常）
    // 形态判定不能看「临时目录里有没有释放好的文件」：那取决于用户有没有正常启动过，
    // 干净机器上 single exe 会被误判成「便携版/蓝牙版」。资源在不在编译期就定了。
    bool HasEmbedded() const { return hasEmbedded_; }
    // 自检模式：只校验了内嵌资源，没有真正写到磁盘
    bool EmbeddedPending() const { return embedPending_; }
    // 自检时发现运行时目录存在但不完整：返回缺的那一项（空=没有这个问题）。
    // 正常启动会自动重建，所以自检只提示、不算失败。
    const std::wstring& EmbeddedStale() const { return embedStale_; }

    // 单文件版：把内嵌的 adb/scrcpy 释放到本地目录，返回该目录（失败返回空）
    std::wstring ExtractEmbeddedTools(bool prune = true);   // 非 static：要记录 embedError_

    // 自检用：「找到文件」不等于「能用」。0 字节或被截断的 exe 依然存在，
    // 界面上却会表现为「点了没反应」。返回空 = 结构校验通过，否则是第一个问题。
    std::wstring VerifyTools();

    // 缓冲变更推迟到「空闲时」执行：
    // 重启 scrcpy 有让 Android 不迁移已有音频流的风险，
    // 所以把这种有风险的操作挪到「手机没在播放」的时刻做，代价最小。
    bool HasPendingBuffer(const std::string& serial) const
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        return pendingBuf_.count(serial) > 0;
    }
    void ApplyPendingBuffer(const std::string& serial);
    // 空闲重置：不改缓冲，只重建通道（预设模式下的定期保养）
    void ApplyIdleReset(const std::string& serial);
    // 该设备是否正在等「延迟重启」——期间不要被断线自愈抢跑，
    // 否则又会出现「刚调完缓冲就没声」（设备端还没释放音频输入）
    bool HasScheduledRestart(const std::string& serial) const
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        for (auto& p : pending_) if (p.serial == serial) return true;
        return false;
    }

    // 诊断：记录「手机在播但电脑端静音」时的现场，供事后定位
    void DumpSilentDiag(const std::string& serial, const std::wstring& extra);

    // 检测「媒体音频被路由到蓝牙」——这是「手机在播但电脑静音」的真正原因：
    // 手机蓝牙还连着电脑，Android 优先把媒体音频送蓝牙，
    // 而程序升级到 ADB 后已断开蓝牙链路 → 音乐被送进黑洞。
    bool IsMediaOnBluetooth(const std::string& serial);

    // 卡顿频繁时自动启用双通道
    void SetAutoDual(bool on) { autoDual_ = on; }
    bool AutoDual() const { return autoDual_; }

    // ---- 双通道热备 ----
    // 实测：同一台设备可以起两个 scrcpy，但 Android 只把音频送给其中一路，
    // 另一路完全静音（REMOTE_SUBMIX Total writes = 0）。
    // 关键发现：杀掉正在出声的那路后，备用路会在约 1 秒内自动接管。
    // 因此可以「常驻一个备用通道」，需要换缓冲/出现卡顿时切过去：
    // 换缓冲是对静音的备用通道做的，完全不影响正在播放的声音。
    void SetDualChannel(bool on);
    bool DualChannel() const { return dualChannel_; }

    // ---- UI 专用读取接口（读快照，绝不等锁）----
    // 工作线程在每次任务/轮询结束时刷新快照；UI 只读快照，
    // 这样即使工作线程正在跑 1.5 秒的 adb 命令，界面也不会卡。
    std::vector<AdbDevice> UiDevices() const;
    std::vector<std::string> UiRunningSerials() const;
    int UiRunningCount() const;
    bool UiIsRunning(const std::string& serial) const;
    int UiBufferMs(const std::string& serial, int defMs) const;
    DWORD UiSpawnMs(const std::string& serial) const;
    int UiDrops(const std::string& serial) const;
    std::map<std::string, int> UiBufferPrefs() const;
    std::set<std::string> UiKnownWireless() const;
    std::set<std::string> UiWantedSerials() const;
    bool UiHasPendingBuffer(const std::string& serial) const;
    // 该设备是否已有「学到的」缓冲值（区别于挡位预设）
    bool UiBufferIsLearned(const std::string& serial) const;
    // 进程 → serial（读快照，供逐设备音量映射用）
    std::string UiSerialForPid(DWORD pid) const;
    // probeOnly=true 时只「定位文件」，不建 Job、不修剪旧目录、不杀孤儿进程。
    // --selftest 用这个：自检不该有副作用（尤其不能把用户正在听的 scrcpy 杀掉）。
    bool Init(const std::wstring& appDir, bool probeOnly = false);
    // 自检模式：释放函数看到不完整目录时不删不写，只校验内嵌资源。
    bool probeOnly_ = false;
    bool hasEmbedded_ = false;      // 本 exe 是否带内嵌工具资源
    bool embedPending_ = false;     // 自检校验通过，但还没真正释放到磁盘
    std::wstring embedStale_;       // 自检时运行时目录存在但不完整（缺哪一项）
    // 探测可用设备：先有线，后无线
    void Probe();
    void DedupRunningSessions();   // 同一台设备多入口时只保留一个在转发
    void PruneBookkeeping();       // 清掉已经不会再出现的 serial 的记账数据
    // 重启 adb server（连带重启它的 mDNS 发现）。
    // 只在用户手动操作、且没有任何会话在跑时调用 —— kill-server 会断掉 adb forward。
    bool RestartAdbServer();
    // 启动/停止 scrcpy 音频转发（单台，兼容旧调用）
    bool Start(const std::string& serial);
    void Stop();

    // ---- 多设备：每台设备一个独立 scrcpy 会话 ----
    // 清理上次异常退出遗留的 scrcpy 进程（避免重复转发/双声）
    void KillOrphans();
    // 程序退出时的收尾：杀掉自己的 scrcpy 子进程，避免退出后还在出声
    void Shutdown();
    // Job Object：父进程被强杀/崩溃时，系统自动杀掉 scrcpy 子进程，
    // 避免「关掉程序后手机声音还在响」
    void SetupKillOnCloseJob();
    bool StartDevice(const std::string& serial, const std::wstring& btName = L"");
    // 断线自愈：把用户要求转发的设备重新建立起来（WiFi 断开/重连、进程崩溃）
    void ReconnectTick();
    // 立刻尝试把已知的无线地址连回来。force=true 时无视「自动重连」开关和 60 秒节流
    // （用户手动点「刷新」时用，等于明确说「我要连上」）。
    void ReconnectKnownWireless(bool force);
    // 把已知但掉线的无线设备重新连上
    void KnownWirelessTick();

    // ---- 自适应延迟：根据 scrcpy 的丢样本日志调节 --audio-buffer ----
    void AdaptTick();
    void SetAutoAdapt(bool on) { autoAdapt_ = on; }
    // 缓冲挡位：0=激进 1=积极 2=标准 3=保守
    void SetBufferGear(int g) { gear_ = (g < 0) ? 0 : (g > 3 ? 3 : g); }
    int BufferGear() const { return gear_; }   // atomic：UI 每帧读，不能等锁
    int GearDefaultMs(bool wireless) const;
    int GearMinMs(bool wireless) const;
    int GearMaxMs(bool wireless) const;
    // 换挡后让运行中的设备按新挡位重启（走延迟重启，避免抢不到音频输入）
    void ApplyGearToRunning();
    // 已知的无线设备 serial（跨启动记住），用于自动重连
    std::set<std::string> KnownWireless() const { return knownWireless_; }
    void SetKnownWireless(const std::set<std::string>& s) { knownWireless_ = s; }
    // 删除某台设备的无线 IP 记录（用户点「删除」时用）。
    // **必须异步**：工作线程做 adb IO 时会把 mtx_ 持好几秒，
    // 在 UI 线程直接抢锁 = 点一次删除冻结好几秒（实测 4.8 秒）。
    void ForgetWirelessAsync(const std::string& serial)
    {
        Post([this, serial]{
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            knownWireless_.erase(serial);
            retryAt_.erase(serial);
            failCount_.erase(serial);
        });
    }
    bool AutoAdapt() const { return autoAdapt_; }
    int BufferFor(const std::string& serial, int defMs = 0) const;

    // 重启后若一直没音频：让手机端媒体流重新建立（暂停→播放）
    // 原因：Android 不会把「已经在播」的媒体流重新路由到新建的 REMOTE_SUBMIX
    void NudgeMedia(const std::string& serial);
    // 手机端媒体是否真的在播放（dumpsys media_session）
    bool IsMediaPlaying(const std::string& serial);
    DWORD SpawnMs(const std::string& serial) const
    {
        auto it = spawnMs_.find(serial);
        return (it != spawnMs_.end()) ? it->second : 0;
    }
    // 「用户希望保持转发」的设备列表（持久化，重启后自动恢复）
    std::set<std::string> WantedSerials() const
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        std::set<std::string> out;
        for (auto& kv : wanted_) out.insert(kv.first);
        return out;
    }
    void SetWantedSerials(const std::set<std::string>& s)
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        for (auto& ser : s) if (!ser.empty()) wanted_[ser] = L"";
    }
    // 这两个会在运行中（UI 线程）被改，工作线程同时也在读 → 必须加锁，
    // 否则 std::map 边读边写会直接把进程搞崩。
    std::map<std::string, int> BufferPrefs() const
    {
        std::lock_guard<std::mutex> lk(prefMtx_);
        return bufPref_;
    }
    void SetBufferPrefs(const std::map<std::string, int>& m)
    {
        std::lock_guard<std::mutex> lk(prefMtx_);
        bufPref_ = m;
    }
    // 按蓝牙名精确定位设备并启动（多设备下不猜 IP）
    bool StartForBtName(const std::wstring& btName);
    // 按 MAC 优先、名字兜底匹配并开始转发。MAC 不随改名变化，是首选依据。
    bool StartForBtDevice(const std::string& btMac, const std::wstring& btName);
    // 无线 IP 变了（DHCP 重新分配）→ 扫网段找回这台设备
    std::string RediscoverWireless(const std::wstring& btName, bool force = false);
    // 标量用 atomic，不要用 mtx_：这个 getter 在 SaveSettings() 和设置面板绘制路径上，
    // 而工作线程做 adb IO 时会持 mtx_ 好几秒 —— 一旦在这里抢锁，界面直接冻住。
    void SetLanScan(bool on) { lanScan_ = on; }
    bool LanScan() const { return lanScan_; }

    // 自动重连（默认开）。关掉后：运行期间设备掉线不再自动连回来
    // —— 用户开关一次手机 WiFi 就能干净地断开，且不会自己复活。
    // 只影响后台的两条复活路径（ReconnectTick / KnownWirelessTick），
    // 启动时的连接和用户手动点「连接」都不受影响。
    void SetAutoReconnect(bool on) { autoReconnect_ = on; }
    bool AutoReconnect() const { return autoReconnect_; }

    // 后台是否允许自动建立连接 / 自动重启会话。
    // 关掉「自动重连」后，只有启动窗口内返回 true；之后一律 false，
    // 直到用户手动点「刷新」（force）或重新启动程序。
    bool AutoConnectAllowed() const
    {
        ULONGLONG now = GetTickCount64();
        return autoReconnect_ || now < startupUntil_.load() || now < manualUntil_.load();
    }
    void StopDevice(const std::string& serial);
    void StopAll();
    bool IsDeviceRunning(const std::string& serial) const;
    // scrcpy 进程 → 设备 serial（逐设备音量用）
    int RunningCount() const;
    std::vector<std::string> RunningSerials() const;

    void Tick();            // 轮询子进程存活、状态
    void AdoptExternal();   // 接管外部已启动的 scrcpy
    AdbState State() const { return state_; }
    std::string Serial() const { return serial_; }
    const std::string& LastError() const { return lastError_; }
    std::vector<AdbDevice> Devices() const { return devices_; }

    // 按设备真实身份去重（同一台设备的 USB + 无线只保留一条，优先有线）
    std::vector<AdbDevice> UniqueDevices() const;       // 会锁 mtx_：只能在 UI 线程**绝不**等锁的地方用
    // UI 线程专用：从快照拿设备再合并，只锁 snapMtx_（工作线程只短暂持有），
    // 绝不去抢 mtx_ —— 工作线程做 adb IO 时会把 mtx_ 持好几秒，UI 一等就是冻结。
    std::vector<AdbDevice> UiUniqueDevices() const;

    // 无线：对已连接的有线设备执行 adb tcpip 5555
    bool ConnectWireless(const std::string& host, int timeoutMs = 8000);   // host = ip:5555

    // 由外部判断 scrcpy 播放出的音频会话（进程名 scrcpy.exe）
    static const wchar_t* SessionProcessName() { return L"scrcpy"; }

    // ---- 蓝牙引导：USB 插入时自动 arm 无线 ADB ----
    // 由主循环定期调用。发现 USB 设备 → 执行 tcpip 5555 → 记录 IP。
    void BootstrapTick();
    bool HasWirelessEndpoint() const { return !wirelessIp_.empty(); }
    void SetWirelessIp(const std::string& ip) { if (!ip.empty()) wirelessIp_ = ip; }
    void ForgetWirelessIp() { wirelessIp_.clear(); }
    std::string WirelessIp() const { return wirelessIp_; }
    // 蓝牙连上后尝试升级到 ADB

    // 查手机端是否真的有音频在播放（dumpsys audio 的 state:started）
    // 比"电脑端 scrcpy 会话活跃"可靠：会话存在 ≠ 手机在出声


private:
    std::wstring adbPath_, scrcpyPath_, scrcpyDir_, serverPath_;
    AdbState state_ = AdbState::Idle;
    std::string serial_;
    std::string lastError_;
    std::vector<AdbDevice> devices_;
    std::string wirelessIp_;          // 已 arm 过的无线 IP
    std::wstring embeddedRoot_;       // 单文件版释放出来的工具根目录（空=分离版或释放失败）
    std::wstring embedError_;         // 释放失败原因（空=没失败）；失败时不能只报「找不到 adb.exe」
    std::map<std::string, std::vector<std::string>> identCache_;  // serial → (identity, model, btName)
    int bootTick_ = 0;
    std::set<std::string> armedSerials_;  // 已经执行过 tcpip 5555 的设备（按设备去重）
    std::vector<AdbSession> sessions_;   // 多设备会话
    std::map<std::string, std::wstring> wanted_;   // 用户希望保持转发的设备 serial → 蓝牙名
    std::map<std::string, int> bufPref_;           // serial → 学到的缓冲 ms（由 prefMtx_ 保护）
    mutable std::mutex prefMtx_;                  // 只保护 bufPref_，绝不在 adb IO 期间持有
    std::map<DWORD, std::shared_ptr<DropBox>> dropBoxes_;   // pid → 丢样本计数器（shared：读线程可安全持有）
    std::map<std::string, int> lastDrops_;         // serial → 上一窗口计数
    std::map<std::string, int> goodWindows_;       // serial → 连续无丢样本文数
    std::map<std::string, int> dropStreak_;        // serial → 连续有丢样本的窗口数
    std::map<std::string, DWORD> lastRestartMs_;   // serial → 上次重启时刻（冷却用）
    std::map<std::string, DWORD> spawnMs_;         // serial → 本次会话启动时刻
    std::map<std::string, DWORD> standbyRetryAt_;  // serial → 备用通道下次允许补位时刻
    int adaptTick_ = 0;
    bool autoAdapt_ = false;   // 默认关闭自适应
    bool shutdownDone_ = false;
    std::atomic<int> gear_{ 2 };   // 默认「标准」
    int reconnectTick_ = 0;
    std::map<std::string, DWORD> retryAt_;      // serial → 下次允许重试的时刻
    std::map<std::string, int> failCount_;      // serial → 连续失败次数（退避用）
    std::set<std::string> knownWireless_;       // 已知无线设备（自动重连用）

    // 保护 devices_ / sessions_ / wanted_ 等共享状态；recursive 因为 tick 之间会互相调用
    mutable std::recursive_mutex mtx_;
    std::thread worker_;
    std::atomic<bool> workerQuit_{ false };
    std::deque<std::function<void()>> queue_;
    std::mutex qmtx_;
    std::atomic<bool> upgradedToAdb_{ false };
    std::map<std::string, int> pendingBuf_;   // serial → 待生效的缓冲值
    bool dualChannel_ = false;
    bool autoDual_ = false;
    int dropEvents5min_ = 0;      // 最近 5 分钟的调整次数
    DWORD dropWindowStart_ = 0;
    std::map<std::string, void*> standby_;    // serial → 备用通道进程
    bool SpawnStandby(const std::string& serial, int bufMs);
    void KillStandby(const std::string& serial);
    void SwitchChannel(const std::string& serial);   // 杀掉当前出声的，让备用接管
    void MaintainStandby();                          // 备用进程死了就补上
    HANDLE killJob_ = nullptr;

    // UI 快照
    struct UiSnapshot
    {
        std::vector<AdbDevice> devices;
        std::vector<std::string> running;
        std::map<std::string, int> bufMs;
        std::map<std::string, DWORD> spawnMs;
        std::map<std::string, int> drops;
        std::map<DWORD, std::string> pidToSerial;
        std::map<std::string, int> bufPref;
        std::set<std::string> knownWireless;
        std::set<std::string> wanted;
        std::set<std::string> pendingBuf;
    };
    mutable std::mutex snapMtx_;
    UiSnapshot snap_;
    void UpdateSnapshot();
    int knownTick_ = 239;   // 故意从 239 起：第一次 tick 就触发（%240==0），
                            // 之后才恢复 60 秒周期。否则启动后要干等一分钟才第一次连无线。
    mutable std::mutex pairMtx_;              // 只保护配对状态，绝不与 mtx_ 混用
    std::wstring pairStatus_;
    std::wstring pairEndpoint_;   // mDNS 找到的配对地址 ip:port
    std::atomic<int> lastRoundTry_{ 0 };      // 上一轮无线重连尝试了几台
    std::atomic<int> lastRoundFail_{ 0 };     // 其中失败了几台
    std::atomic<bool> pairBusy_{ false };
    std::atomic<int>  pairResult_{ 0 };
    std::atomic<int>  upgradeResult_{ 0 };
    // 升级请求防堆积：一次尝试要跑 mDNS(8s) + 扫网段 + 重启 adb + Sleep(1.5s)，
    // 明显长于 UI 的 5 秒重试间隔。不挡的话队列会越堆越多，
    // 工作线程的 Tick/ReconnectTick/AdaptTick 全被饿死。
    std::atomic<bool>      upgradeInFlight_{ false };
    std::atomic<ULONGLONG> upgradeNextAt_{ 0 };   // 失败后退避到这个时间点
    std::wstring upgradeStatus_;
    std::atomic<bool> pairCancel_{ false };   // 用户点了「取消」
    std::atomic<bool> quitting_{ false };     // 正在退出：RunAdb 立刻放弃等待
    std::atomic<bool> lanScan_{ false };     // 是否允许扫网段找回设备（默认关）
    std::atomic<bool> autoReconnect_{ true };  // 运行期间设备掉线是否自动连回来
    // 启动窗口：程序起来后的这一小段时间算「启动阶段」，后台允许连接。
    // 窗口过后，若「自动重连」是关的，就不再有任何后台连接/自愈动作。
    std::atomic<ULONGLONG> startupUntil_{ 0 };
    // 用户点「刷新」后临时授权 60 秒：设备可能几秒后才出现在 adb devices 里，
    // 自动起会话要在这段窗口内还有效，否则「刷新」只连上了通道却不出声。
    std::atomic<ULONGLONG> manualUntil_{ 0 };
    bool SpawnAndTrack(const std::string& serial);

    // 延迟重启：强杀 scrcpy 后设备端 server 需要时间释放音频输入，
    // 立刻重启会抢不到音频流（表现为"重启后无声"）
    struct PendingRestart { std::string serial; DWORD dueMs; };
    std::vector<PendingRestart> pending_;
    void ScheduleRestart(const std::string& serial, DWORD delayMs);
    void ProcessPendingRestarts();
    bool running_ = false;               // 聚合状态：任一会话在跑
    bool externalAdopted_ = false;       // 接管了外部 scrcpy

    std::string RunAdb(const std::string& args, int timeoutMs = 8000);
    std::string QueryWifiIp(const std::string& serial);
    void* SpawnScrcpy(const std::string& serial, int bufOverride = 0);   // 成功返回 PROCESS_INFORMATION*
};
