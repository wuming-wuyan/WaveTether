// ADB 音频通道实现：adb 探测 + scrcpy 纯音频转发
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>      // 必须在 windows.h 之前，否则与 winsock.h 冲突
#include <ws2tcpip.h>

#include "adb.h"
#include "pure.h"
#include "perf.h"
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include <tlhelp32.h>
#include "netscan.h"
// ---- 统一的小工具（消除 6 处重复的进程终止 + 3 处重复的 devices 解析）----

// 终止并释放一个 PROCESS_INFORMATION，指针置空
static void KillPi(void*& slot)
{
    if (!slot) return;
    auto* pi = (PROCESS_INFORMATION*)slot;
    if (pi->hProcess) { TerminateProcess(pi->hProcess, 0); CloseHandle(pi->hProcess); }
    delete pi;
    slot = nullptr;
}

// 解析 `adb devices` 输出。onlyWired=true 时只要不含 ':' 的（USB）设备
static std::vector<std::string> ParseDevices(const std::string& raw, bool onlyWired, bool onlyAuthorized)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < raw.size())
    {
        size_t nl = raw.find('\n', pos);
        std::string line = raw.substr(pos, (nl == std::string::npos) ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? raw.size() : nl + 1;

        if (line.empty() || line[0] == 'L' || line[0] == '*') continue;   // 跳过表头/提示
        size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos) continue;

        std::string ser = line.substr(0, sp);
        std::string st = line.substr(sp);
        while (!st.empty() && (st[0] == ' ' || st[0] == '\t' || st[0] == '\r')) st.erase(0, 1);
        while (!st.empty() && (st.back() == '\r' || st.back() == ' ')) st.pop_back();

        // 只认 adb 真正会报的状态词。
        // 5037 被别的程序占着时，我们拿到的是对方的输出（如 "adb.exe: 5555"），
        // 以前会被当成一台叫 "adb.exe:" 的设备显示在列表里。
        static const char* kStates[] = { "device", "offline", "unauthorized", "bootloader",
                                         "recovery", "sideload", "host", "no permissions",
                                         "authorizing", "connecting", "rescue" };
        bool known = false;
        for (auto k : kStates)
        {
            size_t kl = strlen(k);
            if (st.size() >= kl && _strnicmp(st.c_str(), k, kl) == 0 &&
                (st.size() == kl || st[kl] == ' ')) { known = true; break; }
        }
        if (!known) continue;

        // serial 也不该带空白或冒号收尾（"adb.exe:" 这种垃圾）
        if (ser.empty() || ser.back() == ':' || ser.find(' ') != std::string::npos) continue;

        // mDNS 名字（adb-xxx._adb-tls-connect._tcp）没有冒号，但它是无线。
        // 只按冒号判断会把它当 USB → BootstrapTick 会对它执行 tcpip 5555，
        // 把手机的 adbd 重启、无线调试端口变化、正在播的声音中断。
        bool wired = (ser.find(':') == std::string::npos) &&
                     (ser.find("_adb-tls-") == std::string::npos) &&
                     (ser.find("adb-") != 0);
        if (onlyWired && !wired) continue;
        if (onlyAuthorized && st != "device") continue;
        out.push_back(ser);
    }
    return out;
}





// ================= 单文件版：内嵌工具释放 =================
// 资源打包见 gen_embed.py：每个文件一个 RCDATA（名字是序号），
// 另有 IDR_EMBED_MANIFEST 记录 "序号 \t 相对路径 \t 原大小 \t 压缩后大小"。
// 首次运行释放到 %TEMP%\WaveTether\rt-<内容哈希>\，之后直接复用。

static bool FileExists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
#include "_embedded_tools.inc"

// ================= 后台工作线程 =================
// 目的：adb 调用是同步阻塞的（Probe 100~500ms，BootstrapTick 带 1.5s Sleep，
// 局域网扫描可达 800ms），放在 UI 线程会明显卡顿。这里统一挪到工作线程。

void AdbChannel::Post(std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> lk(qmtx_);
        queue_.push_back(std::move(fn));
    }
}

void AdbChannel::StartWorker()
{
    if (worker_.joinable()) return;
    workerQuit_ = false;
    // 「启动阶段」= 启动后 120 秒。
    // 实测后台轮询一轮是 240 tick ≈ 74 秒（tick 里夹着 adb IO，不是准 250ms），
    // 所以 120 秒能稳稳容纳「立即一次 + 约 74 秒后补一次」两轮尝试；
    // 再往后就完全不动了。
    startupUntil_ = GetTickCount64() + 120000;
    worker_ = std::thread([this]()
    {
        while (!workerQuit_)
        {
            // 异常边界：工作线程里抛出去会直接 std::terminate 整个程序。
            // 抓一次、记一行、睡一下继续跑 —— 单次 Tick 失败不该让用户丢整个界面。
            try
            {
            // 1) 执行 UI 投递的任务
            for (;;)
            {
                std::function<void()> fn;
                {
                    std::lock_guard<std::mutex> lk(qmtx_);
                    if (queue_.empty()) break;
                    fn = std::move(queue_.front());
                    queue_.pop_front();
                }
                if (fn) { fn(); UpdateSnapshot(); }
            }

            // 2) 周期性维护（全部是 adb IO）
            {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                Tick();
                BootstrapTick();
                ReconnectTick();
                KnownWirelessTick();
                DedupRunningSessions();
                {
                    static int pruneTick = 0;
                    if (++pruneTick >= 1200) { pruneTick = 0; PruneBookkeeping(); }
                }
                AdaptTick();
            }
            UpdateSnapshot();
            }
            catch (const std::exception& e) { PerfLog("EXCEPTION worker: %s", e.what()); Sleep(1000); }
            catch (...) { PerfLog("EXCEPTION worker: unknown"); Sleep(1000); }

            Sleep(250);
        }
    });
}

void AdbChannel::StopWorker()
{
    // 先置 quitting_：工作线程可能正卡在 RunAdb 里等一个 20 秒的 adb pair，
    // 不打断的话「退出」按钮会僵住十几秒（实测 19.6 秒）。
    quitting_ = true;
    workerQuit_ = true;
    if (worker_.joinable()) worker_.join();
}

// BT 升级到 ADB：整个过程在工作线程里跑（Probe + StartDevice 都是 IO）
void AdbChannel::RequestUpgradeForBt(const std::wstring& btName, const std::string& btMac)
{
    if (btName.empty()) return;
    // 防堆积：上一次还没跑完、或刚失败还在退避期，就直接不排队。
    if (upgradeInFlight_.exchange(true)) return;
    if (GetTickCount64() < upgradeNextAt_.load()) { upgradeInFlight_ = false; return; }
    std::wstring name = btName;
    std::string mac = btMac;
    upgradeResult_ = 0;
    Post([this, name, mac]()
    {
        // 无论从哪个分支 return，都要复位 in-flight；失败还要退避 20 秒，
        // 避免「每 5 秒重启一次 adb server + 扫一遍全网段」。
        struct InFlightGuard
        {
            AdbChannel* self;
            ~InFlightGuard()
            {
                if (self->upgradeResult_.load() != 1)
                    self->upgradeNextAt_ = GetTickCount64() + 20000;
                self->upgradeInFlight_ = false;
            }
        } inFlightGuard{ this };

        auto setFail = [this](const std::wstring& why) {
            std::lock_guard<std::mutex> lk(pairMtx_);
            upgradeStatus_ = why;
            upgradeResult_ = 2;
        };

        if (StartForBtDevice(mac, name)) { upgradedToAdb_ = true; upgradeResult_ = 1; return; }

        // 没匹配上，按「代价从低到高」依次尝试，不要一上来就扫网段：
        // 扫网段要对 254 个地址做 TCP 连接、再对连上的逐个 adb connect + shell 查蓝牙名，
        // 非必要的扫描会让旁边符合条件的设备都弹出「允许调试」提示。

        // ① 先试已经记录在案的无线地址（改动最小、无副作用）
        std::vector<std::string> known;
        {
            std::lock_guard<std::recursive_mutex> _lk(mtx_);
            for (auto& s2 : knownWireless_) known.push_back(s2);
        }
        for (auto& addr : known)
        {
            if (!ConnectWireless(addr, 5000)) continue;
            Probe();
            if (StartForBtDevice(mac, name)) { upgradedToAdb_ = true; upgradeResult_ = 1; return; }
        }

        // ② 再看 mDNS 有没有广播可直接连的地址（不做网段扫描）
        std::string md = RunAdb("mdns services", 8000);
        std::string mdAddr;
        size_t pos = 0;
        while (pos < md.size())
        {
            size_t nl = md.find(10, pos);
            std::string line = md.substr(pos, (nl == std::string::npos) ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? md.size() : nl + 1;
            if (line.find("_adb-tls-connect") == std::string::npos) continue;
            size_t e = line.size();
            while (e > 0 && (line[e - 1] == ' ' || line[e - 1] == '	' || line[e - 1] == '\r')) --e;
            size_t s3 = e;
            while (s3 > 0 && line[s3 - 1] != ' ' && line[s3 - 1] != '	') --s3;
            std::string hostport = line.substr(s3, e - s3);
            if (hostport.find(':') == std::string::npos) continue;
            if (!ConnectWireless(hostport, 5000)) continue;
            mdAddr = hostport;
            Probe();
            if (StartForBtDevice(mac, name)) { upgradedToAdb_ = true; upgradeResult_ = 1; return; }
        }

        // ③ 前面都不行才扫网段（用户明确点了按钮，允许这一次）
        std::string found = RediscoverWireless(name, /*force=*/true);
        if (!found.empty())
        {
            ConnectWireless(found, 6000);
            Probe();
            if (StartForBtDevice(mac, name)) { upgradedToAdb_ = true; upgradeResult_ = 1; return; }
        }
        // ④ 最后一招：重启 adb server（连带把它失效的 mDNS 发现一起重启），再试一轮。
        //    只在这里做，不做后台维持 —— 用户点了按钮才值得付这个代价。
        if (!RestartAdbServer())
        {
            setFail(L"升级失败：有设备正在转发，重启 adb 会中断它。请先停止其它设备的转发，再点 ↑ADB；"
                    L"或者用「无线调试配对」直接填地址连接。");
            return;
        }
        Sleep(1500);           // 给 mDNS 一点时间重新发现
        Probe();
        if (StartForBtDevice(mac, name)) { upgradedToAdb_ = true; upgradeResult_ = 1; return; }

        {
            std::lock_guard<std::recursive_mutex> _lk(mtx_);
            known.clear();
            for (auto& s2 : knownWireless_) known.push_back(s2);
        }
        for (auto& addr : known)
        {
            if (!ConnectWireless(addr, 5000)) continue;
            Probe();
            if (StartForBtDevice(mac, name)) { upgradedToAdb_ = true; upgradeResult_ = 1; return; }
        }
        std::string md2 = RunAdb("mdns services", 8000);
        pos = 0;
        while (pos < md2.size())
        {
            size_t nl = md2.find(10, pos);
            std::string line = md2.substr(pos, (nl == std::string::npos) ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? md2.size() : nl + 1;
            if (line.find("_adb-tls-connect") == std::string::npos) continue;
            size_t e = line.size();
            while (e > 0 && (line[e - 1] == ' ' || line[e - 1] == '	' || line[e - 1] == '\r')) --e;
            size_t s4 = e;
            while (s4 > 0 && line[s4 - 1] != ' ' && line[s4 - 1] != '	') --s4;
            std::string hostport = line.substr(s4, e - s4);
            if (hostport.find(':') == std::string::npos) continue;
            if (!ConnectWireless(hostport, 5000)) continue;
            Probe();
            if (StartForBtDevice(mac, name)) { upgradedToAdb_ = true; upgradeResult_ = 1; return; }
        }

        setFail(L"升级失败：已试过记录地址、mDNS、局域网扫描，并重启过一次 adb，"
                    L"仍找不到这台设备。请确认手机和电脑在同一网络、手机无线调试已打开；"
                    L"也可以用「无线调试配对」直接填地址连接。");
    });
}


// 创建「关闭即杀子进程」的 Job Object，并把当前进程放进去。
// 之后 CreateProcess 出来的 scrcpy 会自动继承这个 Job；
// 一旦本进程消失（正常退出、被任务管理器强杀、崩溃），句柄关闭 → 子进程被系统杀掉。
void AdbChannel::SetupKillOnCloseJob()
{
    if (killJob_) return;
    killJob_ = CreateJobObjectW(nullptr, nullptr);
    if (!killJob_) return;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(killJob_, JobObjectExtendedLimitInformation, &info, sizeof(info));

    // 若本进程已在别的 Job 里，Win8+ 支持嵌套；失败也不致命，Shutdown() 仍会兜底
    AssignProcessToJobObject(killJob_, GetCurrentProcess());
}


// ================= 双通道热备 =================
// 原理（实测）：同设备两个 scrcpy，Android 只把音频送给其中一路；
// 杀掉出声的那路，备用路约 1 秒接管。
// 用途：把「换缓冲」这种必须重启的操作，放到静音的备用通道上做，不影响播放。

void AdbChannel::SetDualChannel(bool on)
{
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (dualChannel_ == on) return;
    dualChannel_ = on;
    if (!on)
    {
        for (auto& kv : standby_) KillPi(kv.second);
        standby_.clear();
    }
}

bool AdbChannel::SpawnStandby(const std::string& serial, int bufMs)
{
    if (!dualChannel_ || serial.empty()) return false;
    if (standby_.count(serial)) return true;

    void* p = SpawnScrcpy(serial, bufMs);
    if (!p) return false;
    standby_[serial] = p;
    return true;
}

void AdbChannel::KillStandby(const std::string& serial)
{
    auto it = standby_.find(serial);
    if (it == standby_.end()) return;
    KillPi(it->second);
    standby_.erase(it);
}

// 切换：杀掉当前出声的通道 → 备用通道约 1 秒内接管 → 再补一个新备用
void AdbChannel::SwitchChannel(const std::string& serial)
{
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!dualChannel_) return;
    if (!standby_.count(serial)) return;          // 没有备用就不切

    // 1) 杀掉当前出声的（从 sessions_ 移除，但不从 wanted_ 移除）
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it)
    {
        if (it->serial != serial) continue;
        KillPi(it->proc);
        sessions_.erase(it);
        break;
    }

    // 2) 备用提升为正式通道
    auto* spi = (PROCESS_INFORMATION*)standby_[serial];
    standby_.erase(serial);
    sessions_.push_back(AdbSession{ serial, spi, true });
    spawnMs_[serial] = GetTickCount();
    running_ = true;
    state_ = AdbState::Running;

    // 3) 立刻补一个新备用（带上最新缓冲值），它是静音的，不影响播放
    bool wireless = serial.find(':') != std::string::npos;
    SpawnStandby(serial, BufferFor(serial, GearDefaultMs(wireless)));
}

// 备用进程死了就补一个
void AdbChannel::MaintainStandby()
{
    if (!dualChannel_) return;
    // 自动重连关掉 → 不再补备用通道（不凭空新建 ADB 连接）
    if (!autoReconnect_) return;
    DWORD code = 0;
    for (auto& kv : standby_)
    {
        auto* pi = (PROCESS_INFORMATION*)kv.second;
        if (!pi) continue;
        if (WaitForSingleObject(pi->hProcess, 0) == WAIT_OBJECT_0)
        {
            GetExitCodeProcess(pi->hProcess, &code);
            CloseHandle(pi->hProcess);
            delete pi;
            kv.second = nullptr;
            // 备用秒退同样要退避，避免双通道下 4Hz spawn 风暴
            standbyRetryAt_[kv.first] = GetTickCount() + 15000;
        }
    }
    for (auto it = standby_.begin(); it != standby_.end();)
    {
        if (!it->second) it = standby_.erase(it);
        else ++it;
    }
    // 为每个运行中的设备确保有备用
    DWORD now = GetTickCount();
    for (auto& sess : sessions_)
    {
        if (!sess.running) continue;
        if (standby_.count(sess.serial)) continue;
        // 补位也要退避：否则备用进程起不来时会以 4Hz 反复 spawn
        auto itR = standbyRetryAt_.find(sess.serial);
        if (itR != standbyRetryAt_.end() && now < itR->second) continue;

        bool wireless = sess.serial.find(':') != std::string::npos;
        if (!SpawnStandby(sess.serial, BufferFor(sess.serial, GearDefaultMs(wireless))))
            standbyRetryAt_[sess.serial] = now + 15000;
        else
            standbyRetryAt_.erase(sess.serial);
    }
}

// ================= UI 快照 =================
// 工作线程刷新，UI 线程只读。读快照用独立的短锁，绝不会被长任务卡住。

void AdbChannel::UpdateSnapshot()
{
    UiSnapshot tmp;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        tmp.devices = devices_;
        for (auto& s2 : sessions_)
            if (s2.running) tmp.running.push_back(s2.serial);
        { std::lock_guard<std::mutex> lk(prefMtx_); tmp.bufPref = bufPref_; }
        tmp.knownWireless = knownWireless_;
        for (auto& kv : wanted_) tmp.wanted.insert(kv.first);
        for (auto& kv : pendingBuf_) tmp.pendingBuf.insert(kv.first);

        for (auto& s2 : sessions_)
        {
            auto* pi = (PROCESS_INFORMATION*)s2.proc;
            if (!pi) continue;
            tmp.spawnMs[s2.serial] = spawnMs_.count(s2.serial) ? spawnMs_[s2.serial] : 0;
            tmp.pidToSerial[pi->dwProcessId] = s2.serial;
            auto itBox = dropBoxes_.find(pi->dwProcessId);
            if (itBox != dropBoxes_.end())
            {
                tmp.bufMs[s2.serial] = itBox->second->bufMs;
                tmp.drops[s2.serial] = itBox->second->drops.load(std::memory_order_relaxed);
            }
        }
    }
    std::lock_guard<std::mutex> lk(snapMtx_);
    snap_ = std::move(tmp);
}

std::vector<AdbDevice> AdbChannel::UiDevices() const
{
    std::lock_guard<std::mutex> lk(snapMtx_);
    return snap_.devices;
}

std::vector<std::string> AdbChannel::UiRunningSerials() const
{
    std::lock_guard<std::mutex> lk(snapMtx_);
    return snap_.running;
}

int AdbChannel::UiRunningCount() const
{
    std::lock_guard<std::mutex> lk(snapMtx_);
    return (int)snap_.running.size();
}

bool AdbChannel::UiIsRunning(const std::string& serial) const
{
    std::lock_guard<std::mutex> lk(snapMtx_);
    for (auto& s2 : snap_.running) if (s2 == serial) return true;
    return false;
}

int AdbChannel::UiBufferMs(const std::string& serial, int defMs) const
{
    std::lock_guard<std::mutex> lk(snapMtx_);
    auto it = snap_.bufMs.find(serial);
    return (it != snap_.bufMs.end()) ? it->second : defMs;
}

DWORD AdbChannel::UiSpawnMs(const std::string& serial) const
{
    std::lock_guard<std::mutex> lk(snapMtx_);
    auto it = snap_.spawnMs.find(serial);
    return (it != snap_.spawnMs.end()) ? it->second : 0;
}

std::string AdbChannel::UiSerialForPid(DWORD pid) const
{
    if (pid == 0) return {};
    std::lock_guard<std::mutex> lk(snapMtx_);
    auto it = snap_.pidToSerial.find(pid);
    return (it != snap_.pidToSerial.end()) ? it->second : std::string();
}

bool AdbChannel::UiBufferIsLearned(const std::string& serial) const
{
    std::lock_guard<std::mutex> lk(snapMtx_);
    return snap_.bufPref.count(serial) > 0;
}

std::map<std::string, int> AdbChannel::UiBufferPrefs() const
{
    std::lock_guard<std::mutex> lk(snapMtx_);
    return snap_.bufPref;
}

std::set<std::string> AdbChannel::UiKnownWireless() const
{
    std::lock_guard<std::mutex> lk(snapMtx_);
    return snap_.knownWireless;
}

std::set<std::string> AdbChannel::UiWantedSerials() const
{
    std::lock_guard<std::mutex> lk(snapMtx_);
    return snap_.wanted;
}

bool AdbChannel::UiHasPendingBuffer(const std::string& serial) const
{
    std::lock_guard<std::mutex> lk(snapMtx_);
    return snap_.pendingBuf.count(serial) > 0;
}

int AdbChannel::UiDrops(const std::string& serial) const
{
    std::lock_guard<std::mutex> lk(snapMtx_);
    auto it = snap_.drops.find(serial);
    return (it != snap_.drops.end()) ? it->second : 0;
}



// 直接向 adb server（127.0.0.1:5037）要设备列表，**不 spawn adb.exe**。
// 协议：4 位十六进制长度 + 请求体；响应 "OKAY" + 4 位长度 + 内容。
// 实测这是最大的 CPU 来源（每分钟 12 次进程创建），改直连后降到 0。
static std::string AdbServerDevices(int timeoutMs = 1500)
{
    netscan::EnsureWsa();

    SOCKET sk = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sk == INVALID_SOCKET) return {};

    DWORD tv = (DWORD)timeoutMs;
    setsockopt(sk, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
    setsockopt(sk, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof(tv));

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(5037);
    InetPtonA(AF_INET, "127.0.0.1", &a.sin_addr);

    std::string out;
    if (connect(sk, (sockaddr*)&a, sizeof(a)) == 0)
    {
        const char* body = "host:devices";
        int len = (int)strlen(body);
        char req[64];
        sprintf_s(req, "%04x%s", len, body);

        if (send(sk, req, 4 + len, 0) == 4 + len)
        {
            char ok[5]{};
            char hdr[5]{};
            if (recv(sk, ok, 4, 0) == 4 && strncmp(ok, "OKAY", 4) == 0 &&
                recv(sk, hdr, 4, 0) == 4)
            {
                int n = (int)strtol(hdr, nullptr, 16);
                // n==0 表示「adb server 活着但一台设备都没有」——这是最常见状态。
                // 以前要求 n>0，导致零设备时返回空串，调用方以为连不上 server，
                // 退化成每次 RunAdb 建一个 adb.exe（实测 12~16 次/分钟）。
                // 返回表头让调用方知道「可达」，两个解析器都跳过 L 开头的行。
                if (n == 0) out = "List of devices attached\n";
                if (n >= 0 && n < 1024 * 1024)
                {
                    out.reserve(n);
                    while ((int)out.size() < n)
                    {
                        char buf[4096];
                        int want = (std::min)((int)sizeof(buf), n - (int)out.size());
                        int got = recv(sk, buf, want, 0);
                        if (got <= 0) break;
                        out.append(buf, got);
                    }
                }
            }
        }
    }
    closesocket(sk);
    return out;
}

bool AdbChannel::HasServer() const
{
    return !serverPath_.empty() && FileExists(serverPath_);
}

// 自检用：结构上是不是一个完整的 Windows 可执行文件。
// 只判「文件存在」会把 0 字节 / 被截断 / 被杀软换成占位文件的 exe 当成可用，
// 界面上表现成「点了没反应」，而 --selftest 还说全部通过 —— 那种自检等于骗人。
static std::wstring PeProblem(const std::wstring& path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"打不开";

    LARGE_INTEGER sz{};
    bool bigEnough = GetFileSizeEx(f, &sz) && sz.QuadPart >= 4096;

    IMAGE_DOS_HEADER dos{};
    DWORD got = 0;
    bool readDos = ReadFile(f, &dos, sizeof(dos), &got, nullptr) && got == sizeof(dos);
    bool mz = readDos && dos.e_magic == IMAGE_DOS_SIGNATURE;

    bool pe = false;
    if (bigEnough && mz && dos.e_lfanew > 0 && dos.e_lfanew < sz.QuadPart - 4)
    {
        LARGE_INTEGER off{};
        off.QuadPart = dos.e_lfanew;
        if (SetFilePointerEx(f, off, nullptr, FILE_BEGIN))
        {
            DWORD sig = 0;
            if (ReadFile(f, &sig, sizeof(sig), &got, nullptr) && got == 4)
                pe = (sig == IMAGE_NT_SIGNATURE);
        }
    }
    CloseHandle(f);

    if (!bigEnough) return L"文件过小（可能被截断或被占位文件替换）";
    if (!mz || !pe) return L"不是有效的 Windows 可执行文件";
    return std::wstring();
}

static bool NonEmptyFile(const std::wstring& p)
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return false;
    return (((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow) != 0;
}

std::wstring AdbChannel::VerifyTools()
{
    if (adbPath_.empty()) return L"找不到 adb.exe";
    std::wstring p = PeProblem(adbPath_);
    if (!p.empty()) return L"adb.exe " + p;

    if (scrcpyPath_.empty()) return L"找不到 scrcpy.exe";
    p = PeProblem(scrcpyPath_);
    if (!p.empty()) return L"scrcpy.exe " + p;

    // scrcpy-server 是 dex，不是 PE，只要求非空
    if (!HasServer()) return L"找不到 scrcpy-server";
    if (!NonEmptyFile(serverPath_)) return L"scrcpy-server 是空文件";

    // scrcpy 起不来的直接原因通常是依赖 DLL 被隔离：这些名字固定
    const wchar_t* fixed[] = { L"AdbWinApi.dll", L"AdbWinUsbApi.dll",
                               L"libusb-1.0.dll", L"SDL3.dll" };
    for (const wchar_t* n : fixed)
    {
        std::wstring f = scrcpyDir_ + L"\\" + n;
        if (!NonEmptyFile(f)) return std::wstring(L"缺少或为空：") + n;
    }
    // FFmpeg 四个 DLL 带版本号（avcodec-63 等），按前缀找
    const wchar_t* prefixes[] = { L"avcodec-", L"avformat-", L"avutil-", L"swresample-" };
    for (const wchar_t* pfx : prefixes)
    {
        WIN32_FIND_DATAW fd{};
        std::wstring pat = scrcpyDir_ + L"\\" + pfx + L"*.dll";
        HANDLE h = FindFirstFileW(pat.c_str(), &fd);
        bool found = (h != INVALID_HANDLE_VALUE);
        if (found) FindClose(h);
        if (!found || (fd.nFileSizeHigh == 0 && fd.nFileSizeLow == 0))
            return std::wstring(L"缺少或为空：") + pfx + L"*.dll";
    }
    return std::wstring();
}

// ---------------------------------------------------------------- 工具

static std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}


static std::string Trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 调试日志统一写到 %APPDATA%\WaveTether：
// 装到 Program Files 时当前工作目录不可写，写 CWD 会静默失败。
static std::wstring LogPathIn(const wchar_t* name)
{
    static std::wstring dir;
    if (dir.empty())
    {
        wchar_t ap[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"APPDATA", ap, MAX_PATH) > 0)
        {
            dir = std::wstring(ap) + L"\\WaveTether";
            CreateDirectoryW(dir.c_str(), nullptr);
        }
    }
    return dir.empty() ? std::wstring(name) : (dir + L"\\" + name);
}

// ---------------------------------------------------------------- 初始化

bool AdbChannel::Init(const std::wstring& appDirIn, bool probeOnly)
{
    // 先建 Job：之后启动的 scrcpy 都会绑在它上面，本进程消失时被系统清理
    probeOnly_ = probeOnly;
    hasEmbedded_ = FindResourceW(GetModuleHandleW(nullptr), L"IDR_EMBED_MANIFEST", RT_RCDATA) != nullptr;
    if (!probeOnly) SetupKillOnCloseJob();

    // 0) 单文件版：把内嵌的 adb/scrcpy 释放到本地目录（分离版没有资源，返回空）
    std::wstring appDir = appDirIn;
    {
        // probeOnly 时不修剪别的 rt-* 目录（那是写操作，自检不该动别人的东西）
        std::wstring embeddedDir = ExtractEmbeddedTools(!probeOnly);
        if (!embeddedDir.empty())
        {
            embeddedRoot_ = embeddedDir;
            // 释放出来的结构是 <dir>\tools\scrcpy\...，而 appDir 恰好要求同样的层级
            appDir = embeddedDir;
        }
    }

    // 1) adb 只认「随包自带」或 PATH，不再去猜固定安装路径。
    //
    // 以前这里会先试 C:\platform-tools\adb.exe。问题是 C:\ 根目录对普通用户
    // 是可写的（AppendData / 可创建子目录），任何本机用户都能在那个位置放一个
    // 同名 exe，程序启动就会执行它 —— 等于给自己开了一条本地执行路径。
    // 正规安装 platform-tools 的用户，PATH 里本来就有 adb，走下面的 1c 即可。

    // 2) scrcpy：随程序带的 tools\scrcpy\scrcpy-win64-v*/scrcpy.exe
    scrcpyDir_ = appDir + L"\\tools\\scrcpy";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((scrcpyDir_ + L"\\scrcpy-win64-*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            std::wstring dir = scrcpyDir_ + L"\\" + fd.cFileName;
            std::wstring exe = dir + L"\\scrcpy.exe";
            if (FileExists(exe)) { scrcpyPath_ = exe; scrcpyDir_ = dir; break; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (scrcpyPath_.empty())
    {
        std::wstring exe = scrcpyDir_ + L"\\scrcpy.exe";
        if (FileExists(exe)) scrcpyPath_ = exe;
    }
    serverPath_ = scrcpyDir_ + L"\\scrcpy-server";

    // 上次异常退出可能留下 scrcpy 孤儿进程 → 启动时清掉，避免重复转发。
    // 必须放在 scrcpyDir_ / embeddedRoot_ 赋值**之后**：KillOrphans 按 exe 路径
    // 判断是不是本工具链的进程，早调时两个串都还是空的，会一个都不杀。
    // probeOnly 时跳过：--selftest 不能把用户正在听的转发进程杀掉。
    if (!probeOnly) KillOrphans();

    // 1b) 优先用随包发布的 adb：scrcpy 的 Windows 包里自带 adb.exe，
    //     发到别的机器时本机不一定装了 platform-tools。
    if (!scrcpyDir_.empty())
    {
        std::wstring bundled = scrcpyDir_ + L"\\adb.exe";
        if (FileExists(bundled)) adbPath_ = bundled;
    }
    // 1c) 还找不到就试 PATH。
    //
    // 不能用 SearchPathW(nullptr, ...)：它第一个搜的就是「当前目录」，
    // 而当前目录可能是用户双击 exe 时所在的任意位置（下载文件夹、U 盘、网络盘）。
    // 那里放个 adb.exe 就会被执行 —— 等于凭空多一条本地执行路径。
    // 所以自己按 PATH 逐目录找。
    // 1b-2) 便携版场景：用户可能把 adb.exe 直接放在 exe 旁边。
    // SearchPathW 的旧行为第一个就搜「应用目录」，重写时漏了这一条。
    if (adbPath_.empty() && !appDirIn.empty())
    {
        std::wstring cand = appDirIn + L"\adb.exe";
        if (FileExists(cand)) adbPath_ = cand;
    }

    if (adbPath_.empty())
    {
        wchar_t buf[32767]{};
        if (GetEnvironmentVariableW(L"PATH", buf, 32767) > 0)
        {
            std::wstring path = buf;
            size_t p = 0;
            while (p <= path.size() && adbPath_.empty())
            {
                size_t q = path.find(L';', p);
                if (q == std::wstring::npos) q = path.size();
                std::wstring dir = path.substr(p, q - p);
                p = q + 1;
                while (!dir.empty() && dir.back() == L' ') dir.pop_back();
                if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"')
                    dir = dir.substr(1, dir.size() - 2);
                if (dir.empty()) continue;
                std::wstring cand = dir + L"\\adb.exe";
                if (FileExists(cand)) adbPath_ = cand;
            }
        }
    }

    if (adbPath_.empty() && !embedError_.empty())
        lastError_ = "释放运行时文件失败：" + Narrow(embedError_);
    else if (adbPath_.empty())   lastError_ = "找不到 adb.exe";
    else if (scrcpyPath_.empty()) lastError_ = "找不到 scrcpy.exe";
    return !adbPath_.empty();
}

// ---------------------------------------------------------------- adb 调用

std::string AdbChannel::RunAdb(const std::string& args, int timeoutMs)
{
    if (GetEnvironmentVariableW(L"WT_TRACE", nullptr, 0) > 0) {
        FILE* lf=nullptr; _wfopen_s(&lf, LogPathIn(L"adb.log").c_str(), L"a");
        if(lf){ fprintf(lf,"[%lu] %s\n", GetTickCount(), args.c_str()); fclose(lf); }
    }
    if (adbPath_.empty()) return {};

    std::wstring cmd = L"\"" + adbPath_ + L"\" " + Widen(args);
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rPipe = nullptr, wPipe = nullptr;
    if (!CreatePipe(&rPipe, &wPipe, &sa, 0)) return {};
    SetHandleInformation(rPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wPipe;
    si.hStdError = wPipe;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::wstring mut = cmd;
    BOOL ok = CreateProcessW(nullptr, &mut[0], nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wPipe);
    if (!ok) { CloseHandle(rPipe); return {}; }

    std::string out;
    out.reserve(256 * 1024);
    char buf[8192];
    DWORD read = 0;
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeoutMs;   // 64 位，不受 49.7 天回绕影响

    // 持续读到进程退出 + 管道空
    for (;;)
    {
        DWORD avail = 0;
        if (PeekNamedPipe(rPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0)
        {
            while (avail > 0)
            {
                DWORD want = (avail < sizeof(buf)) ? avail : (DWORD)sizeof(buf);
                if (!ReadFile(rPipe, buf, want, &read, nullptr) || read == 0) break;
                out.append(buf, read);
                avail -= read;
            }
            continue;
        }
        DWORD wr = WaitForSingleObject(pi.hProcess, 30);
        if (wr == WAIT_OBJECT_0)
        {
            // 收尾：把剩余全部读出
            while (PeekNamedPipe(rPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0)
            {
                DWORD want = (avail < sizeof(buf)) ? avail : (DWORD)sizeof(buf);
                if (!ReadFile(rPipe, buf, want, &read, nullptr) || read == 0) break;
                out.append(buf, read);
            }
            break;
        }
        if (GetTickCount64() > deadline) break;
        if (quitting_) break;      // 退出中：不再等 adb，直接杀掉子进程返回
    }

    CloseHandle(rPipe);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return out;
}

// ---------------------------------------------------------------- 探测

// 把 "00:11:22:33:44:55" / "00-11-22-33-44-55" / "001122334455" 统一成 12 位大写十六进制。
// 拿不到或格式不对就返回空串，调用方退回按名字匹配。
static std::string NormalizeMac(const std::string& in)
{
    std::string out;
    for (char c : in)
    {
        if (c == ':' || c == '-' || c == ' ') continue;
        if (!isxdigit((unsigned char)c)) return std::string();
        out += (char)toupper((unsigned char)c);
    }
    return out.size() == 12 ? out : std::string();
}

// serial 的「好用程度」：有线 > 普通 ip:port > mDNS 名字（adb-xxx._adb-tls-connect._tcp）
static int SerialRank(const AdbDevice& d)
{
    if (d.isWired) return 3;
    if (d.serial.find("._adb-tls-connect") != std::string::npos) return 1;
    return 2;
}

// 同一台设备（同一 identity）有多个连接入口时，只保留最好的那一个在转发。
// 手机同时开着 tcpip 5555 和 Android 11 无线调试时必然出现这种情况。
// 清理「按 serial 记账」的容器。
// 这些 map 只增不删：手机用无线调试时 serial 里带随机端口，每次重开调试就换一个，
// 长时间运行会一直往里堆永远用不到的条目（内存不多，但都是无效数据）。
bool AdbChannel::RestartAdbServer()
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);

    // 正在转发时绝不能重启：kill-server 会断掉 adb forward，scrcpy 立刻死
    for (auto& se : sessions_)
        if (se.running) return false;

    PerfLog("RESTART 重启 adb server（手动升级的自愈步骤）");
    RunAdb("kill-server", 6000);
    Sleep(1200);
    RunAdb("start-server", 20000);
    identCache_.clear();
    PerfLog("RESTART adb server 已重启");
    return true;
}

void AdbChannel::PruneBookkeeping()
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);

    // 还需要的 serial：在线设备 / 用户希望保持转发 / 已知无线（可能暂时离线，要留着退避）
    std::set<std::string> keep;
    for (auto& d : devices_) keep.insert(d.serial);
    for (auto& kv : wanted_) keep.insert(kv.first);
    for (auto& s2 : knownWireless_) keep.insert(s2);

    auto prune = [&](auto& m) {
        for (auto it = m.begin(); it != m.end();)
            it = keep.count(it->first) ? std::next(it) : m.erase(it);
    };
    prune(spawnMs_);
    prune(lastRestartMs_);
    prune(standbyRetryAt_);
    prune(identCache_);
    prune(lastDrops_);
    prune(goodWindows_);
    prune(dropStreak_);

    // 身份缓存是纯缓存，再加一道上限兜底（防止 serial 快速轮换时短期膨胀）
    if (identCache_.size() > 64) identCache_.clear();
}

void AdbChannel::DedupRunningSessions()
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    std::map<std::string, std::string> keep;      // identity → 保留的 serial

    for (auto& d : devices_)
    {
        if (d.state != "device") continue;
        bool running = false;
        for (auto& s2 : sessions_)
            if (s2.serial == d.serial && s2.running) { running = true; break; }
        if (!running) continue;

        std::string key = d.identity.empty() ? d.serial : d.identity;
        auto it = keep.find(key);
        if (it == keep.end()) { keep[key] = d.serial; continue; }

        // 已经有更好（或同等）的一个在跑 → 停掉这个
        AdbDevice cur{}, best{};
        for (auto& x : devices_) { if (x.serial == d.serial) cur = x; if (x.serial == it->second) best = x; }
        if (SerialRank(cur) > SerialRank(best))
        {
            std::string drop = it->second;
            it->second = d.serial;
            PerfLog("DEDUP 停掉重复入口 %s（同一台设备保留 %s）", drop.c_str(), d.serial.c_str());
            StopDevice(drop);
        }
        else
        {
            PerfLog("DEDUP 停掉重复入口 %s（同一台设备保留 %s）", d.serial.c_str(), it->second.c_str());
            StopDevice(d.serial);
        }
    }
}

void AdbChannel::Probe()
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    devices_.clear();
    if (adbPath_.empty()) { state_ = AdbState::Failed; return; }

    // 正在转发时不要改状态（否则刷新后界面会以为断开了）
    bool wasRunning = (state_ == AdbState::Running);
    if (!wasRunning) state_ = AdbState::Probing;
    // 优先直连 adb server（不 spawn 进程）；失败再退回命令行
    std::string out = AdbServerDevices();
    const char* src = "direct";
    if (out.empty()) { out = RunAdb("devices"); src = "runadb"; }
    {
        // 临时诊断：把 adb devices 的原始输出压成一行记下来
        std::string one;
        for (char c : out) one += (c == 10 || c == 13) ? '|' : c;
        if (one.size() > 300) one = one.substr(0, 300) + "...";
        PerfLog("ADB probe[%s] raw=[%s]", src, one.c_str());
    }
    // 解析：
    // List of devices attached
    // 001122334455   device
    // 192.168.1.5:5555  device
    size_t pos = 0;
    while (pos < out.size())
    {
        size_t nl = out.find('\n', pos);
        std::string line = Trim(out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = (nl == std::string::npos) ? out.size() : nl + 1;
        if (line.empty() || line[0] == 'L' || line[0] == '*') continue;

        size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos) continue;
        AdbDevice d;
        d.serial = line.substr(0, sp);
        d.state = Trim(line.substr(sp));
        // mDNS 名字（adb-xxx._adb-tls-connect._tcp）没有冒号，但它**是无线**。
        // 只按冒号判断会把它当 USB：界面标错、合并时还优先选中它。
        // 粗判（Identify 之后会用 dumpsys 的准确结果覆盖）
        if (d.serial.find("_adb-tls-connect") != std::string::npos) d.pairedState = 1;
        d.isWired = (d.serial.find(':') == std::string::npos) &&
                    (d.serial.find("_adb-tls-") == std::string::npos) &&
                    (d.serial.find("adb-") != 0);
        // 只有「ip:port」形式才值得记：mDNS 名字不能 adb connect，
        // Android 11+ 的随机端口下次也会变（记了只会白白退避一轮）。
        // 记录所有「ip:端口」形式的无线地址，包括无线调试的随机端口。
        // 实测那台平板的无线调试端口 33359 从早到晚都没变过，直接 connect 就能连上；
        // 而 mDNS 发现是坏的（见 perf.log）。所以记住地址比依赖发现更可靠。
        // 代价可控：端口已关时 adb 立即返回「拒绝连接」，不像不可达 IP 要等超时。
        // mDNS 名字不能 adb connect，排除。
        if (!d.isWired && d.state == "device" &&
            d.serial.find(':') != std::string::npos &&
            d.serial.find("_adb-tls-") == std::string::npos)
            knownWireless_.insert(d.serial);
        devices_.push_back(d);
    }

    // 已有无线设备在线时直接采纳，这样拔掉 USB、重启程序后仍能升级到 ADB
    if (wirelessIp_.empty())
        for (auto& d : devices_)
            if (d.state == "device" && !d.isWired)
            {
                size_t c = d.serial.find(':');
                if (c != std::string::npos) wirelessIp_ = d.serial.substr(0, c);
                break;
            }

    // 补设备真实身份（用于合并同一台设备的 USB/无线）
    // 同一 serial 只查一次并缓存，避免每次 Probe 都跑 getprop 卡界面
    for (auto& d : devices_)
    {
        if (d.state != "device") continue;

        auto it = identCache_.find(d.serial);
        if (it != identCache_.end() && it->second.size() == 5)
        {
            d.identity = it->second[0];
            d.model    = it->second[1];
            d.btName   = it->second[2];
            d.btMac    = it->second[3];
            d.pairedState = (it->second[4] == "1") ? 1 : 2;
            continue;
        }

        std::string id = Trim(RunAdb("-s " + d.serial + " shell getprop ro.serialno", 3000));
        std::string md = Trim(RunAdb("-s " + d.serial + " shell getprop ro.product.model", 3000));
        // 设备自己的蓝牙名 —— 和用户在蓝牙列表里看到的名字一致
        std::string bn = Trim(RunAdb("-s " + d.serial + " shell settings get secure bluetooth_name", 3000));
        // 设备自己的蓝牙 MAC —— 改名后仍然不变，是比名字更可靠的身份。
        // 部分 ROM 会限制这个字段，拿不到就留空，调用方自动退回按名字匹配。
        std::string bm = NormalizeMac(Trim(RunAdb("-s " + d.serial + " shell settings get secure bluetooth_address", 3000)));
        // 真相在 dumpsys adb 里：keystore 中出现 adbKey = 无线调试配对过（重启不掉）。
        // mmdns 名字只能说明「这次是怎么连上的」，不能说「配过没配过」。
        std::string dump = RunAdb("-s " + d.serial + " shell dumpsys adb", 4000);
        std::string paired = (dump.find("adbKey") != std::string::npos) ? "1" : "0";
        d.identity = id;
        d.model = md;
        d.btName = bn;
        d.btMac = bm;
        d.pairedState = (paired == "1") ? 1 : 2;
        identCache_[d.serial] = { id, md, bn, bm, paired };
    }

    // 有线优先
    for (auto& d : devices_)
        if (d.isWired && d.state == "device")
        {
            if (!wasRunning) { serial_ = d.serial; state_ = AdbState::Wired; }
            return;
        }
    // 再无线
    for (auto& d : devices_)
        if (!d.isWired && d.state == "device")
        {
            if (!wasRunning) { serial_ = d.serial; state_ = AdbState::Wireless; }
            return;
        }

    if (wasRunning) return;   // 转发中但列表暂时没取到，保持原状态

    serial_.clear();
    state_ = AdbState::Idle;
    if (!devices_.empty()) lastError_ = "设备未授权（请在手机上允许 USB 调试）";
}

bool AdbChannel::ConnectWireless(const std::string& host, int timeoutMs)
{
    std::string out = RunAdb("connect " + host, timeoutMs);
    std::string one;
    for (char c : out) one += (c == 10 || c == 13) ? '|' : c;
    bool ok = out.find("connected") != std::string::npos &&
              out.find("cannot") == std::string::npos &&
              out.find("failed") == std::string::npos;
    PerfLog("ADB connect %s (timeout %d) -> [%s]", host.c_str(), timeoutMs, one.c_str());

    // 连接失败时 adb 仍会把这个地址留在设备列表里，状态是 offline。
    // 而 offline 的设备查不到 serialno，身份是空的 → 去重合并不掉，
    // 界面上就多出一行死地址（实测：192.168.1.100:37527 一直挂着）。
    // 所以失败后主动断开，把这个幽灵条目清掉。
    if (!ok) RunAdb("disconnect " + host, 3000);
    return ok;
}

// ---------------------------------------------------------------- scrcpy

void* AdbChannel::SpawnScrcpy(const std::string& serial, int bufOverride)
{
    if (scrcpyPath_.empty()) { lastError_ = "scrcpy.exe 缺失"; return nullptr; }

    // 缓冲：优先用学到的值，否则无线 200ms / 有线 50ms 起步
    bool wireless = serial.find(':') != std::string::npos;
    int bufMs = (bufOverride > 0) ? bufOverride : BufferFor(serial, GearDefaultMs(wireless));

    // verbosity=debug 是为了拿到 "[Audio] Buffering threshold exceeded" 丢样本日志
    std::wstring args =
        L"--no-window "
        L"--no-control "
        L"--audio-source=playback "
        L"--audio-codec=opus --audio-bit-rate=192K "
        L"--verbosity=debug "
        + (L"--audio-buffer=" + std::to_wstring(bufMs) + L" ");

    if (!serial.empty())
        args = L"-s " + Widen(serial) + L" " + args;

    std::wstring cmd = L"\"" + scrcpyPath_ + L"\" " + args;

    // stderr 管道：统计丢样本
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { lastError_ = "管道创建失败"; return nullptr; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdError = wr;
    si.hStdOutput = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION* pi = new PROCESS_INFORMATION{};
    std::wstring mut = cmd;
    BOOL ok = CreateProcessW(nullptr, &mut[0], nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, &scrcpyDir_[0], &si, pi);
    CloseHandle(wr);
    if (!ok)
    {
        CloseHandle(rd);
        delete pi;
        lastError_ = "scrcpy 启动失败";
        return nullptr;
    }
    CloseHandle(pi->hThread);

    // 后台读 stderr，累计丢样本次数
    auto box = std::make_shared<DropBox>();
    box->bufMs = bufMs;
    std::thread([rd, box]() {
        char buf[4096];
        std::string tail;
        DWORD n = 0;
        const std::string key = "Buffering threshold exceeded";
        while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0)
        {
            tail.append(buf, n);
            size_t pos;
            while ((pos = tail.find(key)) != std::string::npos)
            {
                box->drops.fetch_add(1, std::memory_order_relaxed);
                tail.erase(0, pos + key.size());
            }
            if (tail.size() > 8192) tail.erase(0, tail.size() - 4096);
        }
        CloseHandle(rd);
    }).detach();

    dropBoxes_[pi->dwProcessId] = box;
    return pi;
}

void AdbChannel::ApplyGearToRunning()
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    std::vector<std::string> running = RunningSerials();
    if (running.empty()) return;

    DWORD now = GetTickCount();
    for (auto& ser : running)
    {
        bool wireless = ser.find(':') != std::string::npos;
        { std::lock_guard<std::mutex> lk(prefMtx_); bufPref_.erase(ser); }   // 清掉旧挡位学到的值
        std::wstring bt = wanted_.count(ser) ? wanted_[ser] : L"";
        StopDevice(ser);
        wanted_[ser] = bt;
        lastRestartMs_[ser] = now;
        ScheduleRestart(ser, 4000);                // 延迟重启
        lastDrops_.erase(ser);
        dropStreak_.erase(ser);
        goodWindows_.erase(ser);
        (void)wireless;
    }
}

// 真正执行缓冲变更（重启该设备）。调用方保证此刻「没在播放」。
void AdbChannel::ApplyPendingBuffer(const std::string& serial)
{
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = pendingBuf_.find(serial);
    if (it == pendingBuf_.end()) return;
    if (!IsDeviceRunning(serial)) { pendingBuf_.erase(it); return; }

    DWORD now = GetTickCount();
    if (lastRestartMs_.count(serial) && now - lastRestartMs_[serial] < 30000) return;  // 冷却

    int newBuf = it->second;
    { std::lock_guard<std::mutex> lk(prefMtx_); bufPref_[serial] = newBuf; }
    pendingBuf_.erase(it);
    lastDrops_.erase(serial);

    // 双通道：直接用「已经热好的备用通道」接管（它是现成的，约 1 秒切完），
    // SwitchChannel 内部会再用新缓冲补一个新备用。
    // 注意：千万不要先杀掉备用再新建——新进程要 4-6 秒才注册好 mix，
    // 切过去会得到几秒静音（踩过这个坑）。
    if (dualChannel_ && standby_.count(serial))
    {
        SwitchChannel(serial);
        lastRestartMs_[serial] = now;
        return;
    }

    // 没有双通道：只能重启正在播放的通道
    std::wstring bt = wanted_.count(serial) ? wanted_[serial] : L"";
    StopDevice(serial);
    wanted_[serial] = bt;
    lastRestartMs_[serial] = now;
    ScheduleRestart(serial, 4000);
}


// 媒体音频是否被路由到蓝牙（导致 ADB 抓不到）
bool AdbChannel::IsMediaOnBluetooth(const std::string& serial)
{
    if (serial.empty()) return false;
    std::string out = RunAdb("-s " + serial + " shell dumpsys audio", 5000);
    size_t pos = 0;
    while ((pos = out.find("Devices:", pos)) != std::string::npos)
    {
        size_t e = out.find('\n', pos);
        if (e == std::string::npos) break;
        std::string line = out.substr(pos, e - pos);
        if (line.find("bt_a2dp") != std::string::npos) return true;
        pos = e;
    }
    return false;
}

// 记录静音现场：手机在播、电脑端却一点声音都没有时调用。
// 目的是把定位所需的全部事实一次性落到日志里，避免下次又靠猜。
void AdbChannel::DumpSilentDiag(const std::string& serial, const std::wstring& extra)
{
    std::string out;
    char buf[512];

    snprintf(buf, sizeof(buf), "=== silent diag ===\nserial=%s\n", serial.c_str());
    out += buf;

    // scrcpy 进程是否还在
    out += std::string("session_running=") + (IsDeviceRunning(serial) ? "1" : "0") + "\n";
    out += std::string("dual_channel=") + (dualChannel_ ? "1" : "0") + "\n";
    out += std::string("standby=") + (standby_.count(serial) ? "1" : "0") + "\n";
    {
        std::lock_guard<std::mutex> lk(prefMtx_);
        auto it = bufPref_.find(serial);
        if (it != bufPref_.end()) { snprintf(buf, sizeof(buf), "buf_pref=%d\n", it->second); out += buf; }
    }
    {
        // 不能直接 std::string(w.begin(), w.end())：那是 wchar_t→char 逐字符窄化，
        // 非 ASCII 会被截成乱码（构建时会报 C4244）。诊断日志必须可读。
        out += "extra=" + Narrow(extra) + "\n";
    }

    // 手机端：REMOTE_SUBMIX 有没有数据
    std::string fl = RunAdb("-s " + serial + " shell dumpsys media.audio_flinger", 6000);
    size_t pos = 0;
    int idx = 0;
    while ((pos = fl.find("AUDIO_DEVICE_OUT_REMOTE_SUBMIX", pos)) != std::string::npos && idx < 4)
    {
        size_t w = fl.find("Total writes:", pos);
        if (w == std::string::npos) break;
        size_t e = fl.find('\n', w);
        out += "  submix[" + std::to_string(idx) + "] " + fl.substr(w, e - w) + "\n";
        pos = e; idx++;
    }
    if (idx == 0) out += "  (no REMOTE_SUBMIX output thread)\n";

    // 手机端：媒体输出设备
    std::string au = RunAdb("-s " + serial + " shell dumpsys audio", 6000);
    size_t dp = 0; int dc = 0;
    while ((dp = au.find("Devices:", dp)) != std::string::npos && dc < 3)
    {
        size_t e = au.find('\n', dp);
        out += "  audio " + au.substr(dp, e - dp) + "\n";
        dp = e; dc++;
    }

    out += "\n";
    FILE* f = nullptr;
    _wfopen_s(&f, LogPathIn(L"diag.log").c_str(), L"a");
    if (f) { fwrite(out.c_str(), 1, out.size(), f); fclose(f); }
}

// 空闲重置：只重建通道，不动缓冲值
void AdbChannel::ApplyIdleReset(const std::string& serial)
{
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!IsDeviceRunning(serial)) return;

    DWORD now = GetTickCount();
    if (lastRestartMs_.count(serial) && now - lastRestartMs_[serial] < 60000) return;

    // 有备用通道就切换（更快、更稳），否则走普通重启
    if (dualChannel_ && standby_.count(serial)) { SwitchChannel(serial); lastRestartMs_[serial] = now; return; }

    std::wstring bt = wanted_.count(serial) ? wanted_[serial] : L"";
    StopDevice(serial);
    wanted_[serial] = bt;
    lastRestartMs_[serial] = now;
    ScheduleRestart(serial, 4000);
}

// 退出收尾：杀掉 PC 端子进程。
// scrcpy 断开 socket 后，设备端 server 会自行退出并释放音频输入，
// 所以这里不需要再向设备发命令（那条路径也容易出问题）。
void AdbChannel::Shutdown()
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    if (shutdownDone_) return;
    shutdownDone_ = true;

    StopAll();        // 正常终止自己的会话
    Sleep(300);
    KillOrphans();    // 兜底：清掉可能残留的 scrcpy.exe
}

// 结束所有 scrcpy.exe（本工具链独占，异常退出会留下孤儿）
void AdbChannel::KillOrphans()
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, L"scrcpy.exe") != 0) continue;

            // 只杀本程序工具链里的 scrcpy：按 exe 路径判断。
            // 只比文件名会误杀用户自己开的 scrcpy（同一台机器上很常见）。
            {
                wchar_t exePath[MAX_PATH]{};
                HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (!hp) continue;
                DWORD cap = MAX_PATH;
                BOOL ok = QueryFullProcessImageNameW(hp, 0, exePath, &cap);
                CloseHandle(hp);
                if (!ok) continue;

                bool mine = false;
                for (const std::wstring* d : { &scrcpyDir_, &embeddedRoot_ })
                {
                    if (d->empty()) continue;
                    // 必须命中到目录边界：否则 scrcpy-win64-v5.0.1X 会被当成
                    // scrcpy-win64-v5.0.1 的子路径误杀
                    if (_wcsnicmp(exePath, d->c_str(), d->size()) == 0 &&
                        (exePath[d->size()] == L'\\' || exePath[d->size()] == L'/'))
                    {
                        mine = true; break;
                    }
                }
                if (!mine) continue;
            }

            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
            if (h) { TerminateProcess(h, 0); CloseHandle(h); }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

// 真正启动（不改变「希望保持」集合）

// ---- 自适应延迟 ----


// ---- 缓冲挡位参数 ----
// 挡位越高，允许的缓冲越大、默认值越保守
int AdbChannel::GearDefaultMs(bool wireless) const
{
    // 无线预设依据实测：手机 250ms 才完全不丢样本，平板 150ms 就够。
    // 取折中，让多数场景一次到位，减少自适应介入。
    static const int w[] = { 80, 150, 220, 320 };
    static const int c[] = { 40,  70, 110, 160 };
    int i = (gear_ < 0) ? 0 : (gear_ > 3 ? 3 : gear_);
    return wireless ? w[i] : c[i];
}

int AdbChannel::GearMinMs(bool wireless) const
{
    static const int w[] = { 50,  80, 120, 150 };
    static const int c[] = { 30,  40,  60,  90 };
    int i = (gear_ < 0) ? 0 : (gear_ > 3 ? 3 : gear_);
    return wireless ? w[i] : c[i];
}

int AdbChannel::GearMaxMs(bool wireless) const
{
    static const int w[] = { 260, 360, 460, 700 };
    static const int c[] = { 160, 220, 280, 400 };
    int i = (gear_ < 0) ? 0 : (gear_ > 3 ? 3 : gear_);
    return wireless ? w[i] : c[i];
}

int AdbChannel::BufferFor(const std::string& serial, int defMs) const
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    std::lock_guard<std::mutex> lk(prefMtx_);
    auto it = bufPref_.find(serial);
    return (it != bufPref_.end()) ? it->second : defMs;
}

// 每 2 秒评估一次：丢样本多就加大缓冲并重启该设备；长期干净就逐步降低
void AdbChannel::AdaptTick()
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    if (!autoAdapt_) return;
    if (++adaptTick_ % 8 != 0) return;      // 250ms tick → 2 秒

    const int kWin = 2;                      // 2 秒窗口

    // 注意：这里不能在遍历 sessions_ 时调用 StopDevice（它会 erase），
    // 否则迭代器失效是未定义行为。先把决策收集起来，循环结束后再执行。
    struct Action { std::string serial; int nextMs; };
    std::vector<Action> actions;

    for (auto& sess : sessions_)
    {
        if (!sess.running) continue;
        auto* pi = (PROCESS_INFORMATION*)sess.proc;
        if (!pi) continue;

        auto itBox = dropBoxes_.find(pi->dwProcessId);
        if (itBox == dropBoxes_.end()) continue;

        int total = itBox->second->drops.load(std::memory_order_relaxed);
        int prev = lastDrops_[sess.serial];
        int delta = total - prev;
        lastDrops_[sess.serial] = total;

        bool wireless = sess.serial.find(':') != std::string::npos;
        // cur 必须用「进程实际生效的值」，不能用 bufPref_（那是下次才生效的意图）
        int cur = itBox->second->bufMs;
        if (cur <= 0) cur = GearDefaultMs(wireless);
        int minMs = GearMinMs(wireless);
        int maxMs = GearMaxMs(wireless);

        if (delta > 0)
        {
            goodWindows_[sess.serial] = 0;
            int& streak = dropStreak_[sess.serial];
            if (delta >= 2) streak++; else streak = 0;

            // 连续两个窗口都在丢（约 4 秒）才上调，避免偶发抖动把延迟推高
            if (streak >= 2 && cur < maxMs)
            {
                streak = 0;
                int stepUp = (std::max)(10, (int)(cur * 0.20f));
                actions.push_back({ sess.serial, (std::min)(cur + stepUp, maxMs) });
            }
        }
        else
        {
            int& good = goodWindows_[sess.serial];
            good += kWin;
            // 连续 60 秒完全没有丢样本 → 降一点，换更低延迟
            if (good >= 60 && cur > minMs)
            {
                good = 0;
                int stepDown = (std::max)(5, (int)(cur * 0.10f));
                actions.push_back({ sess.serial, (std::max)(cur - stepDown, minMs) });
            }
        }
    }

    // 只挂起，不立刻重启。
    // 重启 scrcpy 会让 Android 新建 AudioPolicyMix，已在播的流可能不被迁移
    // → 手机还在播、电脑静音。所以把重启推迟到「空闲时」（见 ApplyPendingBuffer）。
    // 统计 5 分钟内的调整次数；太频繁说明链路不稳 → 自动开双通道
    if (!actions.empty())
    {
        DWORD now3 = GetTickCount();
        if (dropWindowStart_ == 0 || now3 - dropWindowStart_ > 5 * 60 * 1000)
        { dropWindowStart_ = now3; dropEvents5min_ = 0; }
        dropEvents5min_ += (int)actions.size();
        if (autoDual_ && !dualChannel_ && dropEvents5min_ >= 3)
        {
            dualChannel_ = true;
            lastError_ = "卡顿频繁，已自动启用双通道热备";
        }
    }

    for (auto& a : actions)
    {
        { std::lock_guard<std::mutex> lk(prefMtx_); bufPref_[a.serial] = a.nextMs; }
        pendingBuf_[a.serial] = a.nextMs;
        lastError_ = "空闲时切换到 " + std::to_string(a.nextMs) + "ms";
        dropStreak_.erase(a.serial);
        goodWindows_.erase(a.serial);
    }

    // 清理已消失会话的统计
    for (auto it = lastDrops_.begin(); it != lastDrops_.end();)
    {
        if (!IsDeviceRunning(it->first)) it = lastDrops_.erase(it);
        else ++it;
    }
}


void AdbChannel::ScheduleRestart(const std::string& serial, DWORD delayMs)
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    for (auto& p : pending_)
        if (p.serial == serial) { p.dueMs = GetTickCount() + delayMs; return; }
    pending_.push_back({ serial, GetTickCount() + delayMs });
}

// 让手机端媒体流重新建立。
// 关键：只发 play 是没用的 —— 对「已经在播」的会话是空操作，
// Android 不会把已有的 AudioTrack 迁移到新建的 AudioPolicyMix。
// 必须「暂停 → 播放」强制 App 销毁并重建 AudioTrack，才会被重新路由。
void AdbChannel::NudgeMedia(const std::string& serial)
{
    if (serial.empty()) return;
    RunAdb("-s " + serial + " shell cmd media_session dispatch pause", 5000);
    Sleep(600);
    RunAdb("-s " + serial + " shell cmd media_session dispatch play", 5000);
}

// 手机端是否有媒体在播放（用于区分「真的没在放」和「在放但没转发过来」）
bool AdbChannel::IsMediaPlaying(const std::string& serial)
{
    if (serial.empty()) return false;
    std::string out = RunAdb("-s " + serial + " shell dumpsys media_session", 5000);
    return HasPlayingState(out);
}

void AdbChannel::ProcessPendingRestarts()
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    if (pending_.empty()) return;
    DWORD now = GetTickCount();
    for (auto it = pending_.begin(); it != pending_.end();)
    {
        if (now < it->dueMs) { ++it; continue; }
        std::string ser = it->serial;
        it = pending_.erase(it);
        if (wanted_.count(ser)) SpawnAndTrack(ser);
    }
}

bool AdbChannel::SpawnAndTrack(const std::string& serial)
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    for (auto& s : sessions_)
        if (s.serial == serial && s.running) return true;

    void* p = SpawnScrcpy(serial);
    if (!p)
    {
        state_ = sessions_.empty() ? AdbState::Failed : state_;
        return false;
    }

    sessions_.push_back(AdbSession{ serial, p, true });
    spawnMs_[serial] = GetTickCount();
    running_ = true;
    state_ = AdbState::Running;
    if (serial_.empty()) serial_ = serial;
    lastError_.clear();
    return true;
}

bool AdbChannel::StartDevice(const std::string& serial, const std::wstring& btName)
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    if (serial.empty()) { lastError_ = "没有可用设备"; return false; }
    wanted_[serial] = btName;      // 记住：这是用户希望保持转发的设备
    return SpawnAndTrack(serial);
}

// 断线自愈：每 5 秒检查一次，把「希望转发但没在跑」的设备重新建立起来
// 周期性把「已知但当前没连上」的无线设备重新连回来。
// 用户常见困惑：平板明明在线，却不在列表里 —— 因为 adb 那边掉线后没人再连它。
void AdbChannel::KnownWirelessTick() { ReconnectKnownWireless(false); }

void AdbChannel::ReconnectKnownWireless(bool force)
{
    // 自动重连关掉 → 后台这条复活路径不跑。
    // 例外：force=true（用户手动点「刷新」）。
    if (!force && !AutoConnectAllowed()) return;
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    if (knownWireless_.empty())
    {
        static bool loggedEmpty = false;
        if (!loggedEmpty) { loggedEmpty = true; PerfLog("ADB KnownWireless: 集合为空，永不尝试无线重连"); }
        return;
    }
    if (!force && ++knownTick_ % 240 != 0) return;   // 250ms tick → 60 秒
    if (force)
    {
        retryAt_.clear(); failCount_.clear();               // 手动点刷新：退避归零，立刻试
        manualUntil_ = GetTickCount64() + 60000;            // 顺带给自动起会话开 60 秒口子
    }
    PerfLog("ADB KnownWireless: 第 %d 次触发，已知 %zu 个 IP", knownTick_, knownWireless_.size());

    bool changed = false;
    DWORD now = GetTickCount();

    // 先筛出「需要尝试」的候选，再按失败次数排序。
    // 原来的写法有两个坑：
    //   1) 每轮只试一台（triedThisTick break），3 个 IP 要分 3 轮 = 3 分钟起
    //   2) std::set 字典序把 192.168.137.x 排在 192.168.9.x 前面，
    //      陈旧的失效 IP 一直占着唯一的尝试名额，后面的设备永远轮不上
    std::vector<std::string> cand;
    for (auto& ser : knownWireless_)
    {
        // 只要还在 adb devices 里（哪怕 offline / unauthorized）就不算「连不上」。
        // 以前只认 state=="device"，USB 连着但无线记录陈旧时会把它当失败，
        // 一小时后把用户正在用的设备记录删掉。
        bool present = false;
        for (auto& d : devices_)
            if (d.serial == ser) { present = true; break; }
        if (present) { failCount_.erase(ser); retryAt_.erase(ser); continue; }

        auto itR = retryAt_.find(ser);
        if (itR != retryAt_.end() && now < itR->second)
        {
            PerfLog("ADB KnownWireless: %s 退避中，还剩 %lu ms", ser.c_str(),
                    (unsigned long)(itR->second - now));
            continue;
        }
        cand.push_back(ser);
    }

    // 没失败过的排前面：宁可先试新 IP，也不要反复喂一个死 IP
    std::stable_sort(cand.begin(), cand.end(), [this](const std::string& a, const std::string& b) {
        auto fa = failCount_.find(a), fb = failCount_.find(b);
        return (fa == failCount_.end() ? 0 : fa->second) < (fb == failCount_.end() ? 0 : fb->second);
    });

    // 一轮最多试 4 台 / 总预算 8 秒：够覆盖多设备，又不会长时间持着 mtx_
    int tried = 0, failed = 0;
    DWORD budgetStart = now;
    for (auto& ser : cand)
    {
        if (tried >= 4) break;
        if (GetTickCount() - budgetStart > 8000) { PerfLog("ADB KnownWireless: 本轮预算用完，剩 %zu 台下轮再试", cand.size() - tried); break; }
        ++tried;
        if (ConnectWireless(ser, 2500)) { changed = true; failCount_.erase(ser); retryAt_.erase(ser); }
        else
        {
            ++failed;
            int& fails = failCount_[ser];
            fails = (std::min)(fails + 1, 6);
            retryAt_[ser] = now + (DWORD)(std::min)(60000u << (fails - 1), 1800000u);

            // 以前这里「连续失败 6 次就删掉这条 IP」。问题是很久没开机、或者
            // mDNS 恰好失效时，地址会被删掉，等设备真回来了反而没有地址可试。
            // 改成：不删单条，只在总量超限时按失败次数淘汰最差的那条。
            if (knownWireless_.size() > 12)
            {
                std::string worst;
                int worstFails = -1;
                for (auto& kv : failCount_)
                {
                    if (!knownWireless_.count(kv.first)) continue;
                    if (kv.second > worstFails) { worstFails = kv.second; worst = kv.first; }
                }
                if (!worst.empty() && worst != ser)
                {
                    PerfLog("ADB KnownWireless: 超过 12 条，淘汰失败最多的 %s（%d 次）", worst.c_str(), worstFails);
                    knownWireless_.erase(worst);
                    failCount_.erase(worst);
                    retryAt_.erase(worst);
                }
            }
        }
    }
    lastRoundTry_ = tried;
    lastRoundFail_ = failed;
    if (changed) { identCache_.clear(); Probe(); }
}

void AdbChannel::ReconnectTick()
{
    // 断线自愈：只看开关，不享受启动窗口 —— 用户要的就是「掉线就停在那儿」。
    if (!autoReconnect_) return;
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    if (wanted_.empty())
    {
        static bool loggedWantedEmpty = false;
        if (!loggedWantedEmpty) { loggedWantedEmpty = true; PerfLog("ADB Reconnect: wanted_ 为空，断线自愈不工作"); }
        return;
    }
    if (++reconnectTick_ % 20 != 0) return;    // 250ms tick → 5 秒
    PerfLog("ADB Reconnect: 第 %d 次触发，wanted_=%zu 个", reconnectTick_, wanted_.size());

    bool needProbe = false;

    // 不能在遍历 wanted_ 时 erase（迭代器失效），先收集要改名的条目
    std::vector<std::pair<std::string, std::string>> renamed;   // (旧 serial, 新 serial)
    std::vector<std::string> toStart;

    DWORD now = GetTickCount();
    // 同 KnownWirelessTick：持着 mtx_ 做同步 adb 调用，一轮最多处理一台，
    // 否则 N 台设备全掉线时界面会连续冻结 N × (connect 超时 + 网段扫描)。
    bool handledOne = false;
    for (auto& kv : wanted_)
    {
        if (handledOne) break;
        const std::string& ser = kv.first;
        if (IsDeviceRunning(ser)) { retryAt_.erase(ser); continue; }
        // 正在等延迟重启就跳过本次（否则 4 秒静默期会被这里抢跑）
        bool waiting = false;
        for (auto& p : pending_) if (p.serial == ser) { waiting = true; break; }
        if (waiting) continue;

        // 退避：设备长时间离线时不要每 5 秒无脑重启（会不停泄漏 DropBox、界面状态闪烁）
        auto itRetry = retryAt_.find(ser);
        if (itRetry != retryAt_.end() && now < itRetry->second) continue;

        // 无线设备：先确认 adb 通道还在，不在就重连
        if (ser.find(':') != std::string::npos)
        {
            bool present = false;
            for (auto& d : devices_)
                if (d.serial == ser && d.state == "device") { present = true; break; }

            if (!present)
            {
                handledOne = true;
                if (!ConnectWireless(ser, 3000))
                {
                    // 还是连不上 → 可能 IP 变了，按蓝牙名在网段内找一次
                    if (!kv.second.empty() && lanScan_)
                    {
                        std::string found = RediscoverWireless(kv.second);
                        if (!found.empty() && found != ser)
                        {
                            renamed.push_back({ ser, found });
                            toStart.push_back(found);
                            needProbe = true;
                            continue;
                        }
                    }
                    // 连不上：指数退避（5s→10s→20s→40s→80s→160s→300s 上限）
                    int& fails = failCount_[ser];
                    fails = (std::min)(fails + 1, 8);
                    DWORD wait = (DWORD)(std::min)(5000u << (fails - 1), 300000u);
                    retryAt_[ser] = now + wait;
                    continue;
                }
                Sleep(300);
                needProbe = true;
            }
        }
        else
        {
            // 有线设备：确认还在 adb devices 里，拔线后不要反复尝试启动
            bool present = false;
            for (auto& d : devices_)
                if (d.serial == ser && d.state == "device") { present = true; break; }
            if (!present)
            {
                int& fails = failCount_[ser];
                fails = (std::min)(fails + 1, 8);
                DWORD wait = (DWORD)(std::min)(5000u << (fails - 1), 300000u);
                retryAt_[ser] = now + wait;
                continue;
            }
        }

        toStart.push_back(ser);
    }

    // 统一执行改名（IP 变化）
    for (auto& r : renamed)
    {
        auto it = wanted_.find(r.first);
        if (it == wanted_.end()) continue;
        wanted_[r.second] = it->second;
        wanted_.erase(it);
    }

    for (auto& ser : toStart)
    {
        if (SpawnAndTrack(ser)) { retryAt_.erase(ser); failCount_.erase(ser); }
        else
        {
            int& fails = failCount_[ser];
            fails = (std::min)(fails + 1, 8);
            retryAt_[ser] = now + (DWORD)(std::min)(5000u << (fails - 1), 300000u);
        }
    }

    if (needProbe) { identCache_.clear(); Probe(); }
}

void AdbChannel::StopDevice(const std::string& serial)
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    wanted_.erase(serial);
    KillStandby(serial);
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it)
    {
        if (it->serial != serial) continue;
        KillPi(it->proc);
        sessions_.erase(it);
        break;
    }

    running_ = !sessions_.empty();
    if (!running_)
    {
        state_ = AdbState::Idle;
        serial_.clear();
    }
    else
    {
        serial_ = sessions_.front().serial;
    }
}

void AdbChannel::StopAll()
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    wanted_.clear();
    for (auto& kv : standby_) KillPi(kv.second);
    standby_.clear();
    for (auto& s : sessions_)
    {
        if (!s.proc) continue;
        auto* pi = (PROCESS_INFORMATION*)s.proc;
        TerminateProcess(pi->hProcess, 0);
        CloseHandle(pi->hProcess);
        delete pi;
    }
    sessions_.clear();
    externalAdopted_ = false;
    running_ = false;
    serial_.clear();
    state_ = AdbState::Idle;
    lastError_.clear();
}

// 兼容旧调用：Start = 只留这一台
bool AdbChannel::Start(const std::string& serial)
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    StopAll();
    return StartDevice(serial);
}

void AdbChannel::Stop() { StopAll(); }

// wchar_t → UTF-8（adb shell 输出是 UTF-8，必须同编码比较）
static std::string ToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

// 无线 IP 变了 → 在本地网段扫描 5555，按蓝牙名找回这台设备
std::string AdbChannel::RediscoverWireless(const std::wstring& btName, bool force)
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    // 自动重连才看这个开关；用户手动点「↑ADB」时 force=true，应该允许扫一次
    if (!lanScan_ && !force) return {};
    std::string hint = wirelessIp_;
    if (hint.empty()) return {};

    auto ips = netscan::ScanAdbPorts(hint, 600);
    if (ips.empty()) return {};

    std::string want = ToUtf8(btName);
    for (auto& ip : ips)
    {
        std::string target = ip + ":5555";
        if (target == hint + ":5555") continue;

        if (!ConnectWireless(target)) continue;

        std::string bn = Trim(RunAdb("-s " + target + " shell settings get secure bluetooth_name", 3000));
        if (!want.empty() && bn == want)
        {
            wirelessIp_ = ip;      // 记住新 IP
            return target;
        }
        RunAdb("disconnect " + target, 3000);   // 不是这台，断开
    }
    return {};
}

// 按蓝牙名找到对应设备并启动（多设备下精确定位，不猜 IP）
bool AdbChannel::StartForBtDevice(const std::string& btMac, const std::wstring& btName)
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    if (devices_.empty()) Probe();

    // ① 按 MAC 匹配：改名不影响
    if (!btMac.empty())
    {
        for (auto& d : devices_)
        {
            if (d.state != "device" || d.btMac.empty()) continue;
            if (d.btMac != btMac) continue;
            return StartDevice(d.serial, btName);
        }
    }
    // ② MAC 拿不到（部分 ROM 限制）或没匹配上 → 退回按名字
    return StartForBtName(btName);
}

bool AdbChannel::StartForBtName(const std::wstring& btName)
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    if (btName.empty()) return false;

    if (devices_.empty()) Probe();

    std::string want = ToUtf8(btName);
    for (auto& d : devices_)
    {
        if (d.state != "device" || d.btName.empty()) continue;
        if (d.btName != want) continue;
        return StartDevice(d.serial, btName);
    }

    // 没找到 → 可能是无线 IP 变了，扫网段重发现一次
    std::string target = RediscoverWireless(btName);
    if (!target.empty())
    {
        identCache_.clear();     // 新连接，身份缓存作废
        Probe();
        return StartDevice(target, btName);
    }
    return false;
}

bool AdbChannel::IsDeviceRunning(const std::string& serial) const
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    for (auto& s : sessions_)
        if (s.serial == serial && s.running) return true;
    return false;
}

int AdbChannel::RunningCount() const
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    int n = 0;
    for (auto& s : sessions_) if (s.running) n++;
    return n;
}

std::vector<std::string> AdbChannel::RunningSerials() const
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    std::vector<std::string> out;
    for (auto& s : sessions_) if (s.running) out.push_back(s.serial);
    return out;
}

// 是否已有外部启动的 scrcpy（用户手动跑的，或上一版残留）
// CreateToolhelp32Snapshot 实测 7.8ms/次（本机 300+ 进程），而这个函数被
// Tick()（4Hz）和 AdoptExternal()（4Hz）同时调用 → 62ms/s，空闲 CPU 8%。
// 结果缓存 3 秒：外部 scrcpy 被接管晚几秒无感，CPU 直接降回 2%。
static bool FindExternalScrcpy()
{
    static ULONGLONG lastAt = 0;
    static bool lastVal = false;
    ULONGLONG now = GetTickCount64();
    if (lastAt != 0 && now - lastAt < 3000) return lastVal;
    lastAt = now;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { lastVal = false; return false; }
    PROCESSENTRY32W pe{ sizeof(pe) };
    bool found = false;
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, L"scrcpy.exe") == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    lastVal = found;
    return found;
}

void AdbChannel::AdoptExternal()
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    if (!sessions_.empty() || !FindExternalScrcpy()) return;

    externalAdopted_ = true;
    running_ = true;
    state_ = AdbState::Running;
    lastError_.clear();

    // 外部接管时 serial_ 可能为空，补上，否则后续判断拿不到设备
    if (serial_.empty())
    {
        for (auto& d : devices_)
            if (d.state == "device") { serial_ = d.serial; break; }
    }
    if (serial_.empty())
    {
        Probe();
        if (state_ == AdbState::Running) return;
        if (!serial_.empty()) state_ = AdbState::Running;
    }
}

void AdbChannel::Tick()
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    ProcessPendingRestarts();
    MaintainStandby();

    // 清理已退出的会话
    for (auto it = sessions_.begin(); it != sessions_.end();)
    {
        auto* pi = (PROCESS_INFORMATION*)it->proc;
        if (!pi) { it = sessions_.erase(it); continue; }

        if (WaitForSingleObject(pi->hProcess, 0) == WAIT_OBJECT_0)
        {
            DWORD code = 0;
            GetExitCodeProcess(pi->hProcess, &code);
            dropBoxes_.erase(pi->dwProcessId);   // 修泄漏：原来只 insert 从不 erase
            DWORD lived = GetTickCount() - (spawnMs_.count(it->serial) ? spawnMs_[it->serial] : 0);
            std::string ser = it->serial;

            CloseHandle(pi->hProcess);
            delete pi;
            it = sessions_.erase(it);

            // 起来不到 3 秒就退出 = 启动失败（参数不兼容/设备不支持/端口占用）。
            // 这种情况不能每 5 秒无脑重试（会刷屏式 spawn + 泄漏 DropBox），
            // 改成指数退避并明确告诉用户原因。
            if (lived < 3000)
            {
                int& fails = failCount_[ser];
                fails = (std::min)(fails + 1, 6);
                DWORD wait = (DWORD)(std::min)(30000u << (fails - 1), 600000u);
                retryAt_[ser] = GetTickCount() + wait;
                lastError_ = "scrcpy 启动后立即退出(code " + std::to_string(code) + ")";
            }
            continue;
        }
        ++it;
    }

    // 自己没会话时，看看是否有外部 scrcpy 在跑
    if (sessions_.empty())
    {
        running_ = false;
        serial_.clear();
        if (!externalAdopted_ && state_ == AdbState::Running) state_ = AdbState::Idle;
        bool ext = FindExternalScrcpy();
        if (externalAdopted_ && !ext)
        {
            externalAdopted_ = false;
            running_ = false;
            state_ = AdbState::Idle;
        }
        else if (!externalAdopted_ && ext)
        {
            externalAdopted_ = true;
            running_ = true;
            state_ = AdbState::Running;
        }
        return;
    }

    running_ = true;
    state_ = AdbState::Running;
    serial_ = sessions_.front().serial;
}


// 合并同一条设备的 USB / 无线两条记录（按 identity，优先保留有线）。
// 纯计算，不碰任何锁。
static std::vector<AdbDevice> MergeSameDevice(const std::vector<AdbDevice>& in)
{
    std::vector<AdbDevice> out;
    for (auto& d : in)
    {
        if (d.state != "device") { out.push_back(d); continue; }
        std::string key = d.identity.empty() ? d.serial : d.identity;

        bool merged = false;
        for (auto& o : out)
        {
            std::string okey = o.identity.empty() ? o.serial : o.identity;
            if (okey != key) continue;
            merged = true;
            // 同一台设备的多个入口里，只要有一条判定为「已配对」就算已配对
            int pa = (std::max)(o.pairedState == 1 ? 1 : 0, d.pairedState == 1 ? 1 : 0);
            if (pa == 0) pa = (o.pairedState || d.pairedState) ? 2 : 0;
            if (SerialRank(d) > SerialRank(o)) o = d;
            o.pairedState = pa;
            break;
        }
        if (!merged) out.push_back(d);
    }
    return out;
}

std::vector<AdbDevice> AdbChannel::UiUniqueDevices() const
{
    std::vector<AdbDevice> raw;
    {
        std::lock_guard<std::mutex> lk(snapMtx_);
        raw = snap_.devices;
    }
    return MergeSameDevice(raw);
}

std::vector<AdbDevice> AdbChannel::UniqueDevices() const
{
    std::lock_guard<std::recursive_mutex> _lk(mtx_);
    return MergeSameDevice(devices_);
}

void AdbChannel::RefreshPairingEndpointAsync()
{
    Post([this]()
    {
        std::string out = RunAdb("mdns services", 8000);
        std::string found;
        size_t pos = 0;
        while (pos < out.size())
        {
            size_t nl = out.find(10, pos);
            std::string line = out.substr(pos, (nl == std::string::npos) ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? out.size() : nl + 1;
            if (line.find("_adb-tls-pairing") == std::string::npos) continue;

            // 行格式（TAB 分隔）：<实例名>	_adb-tls-pairing._tcp	<ip:port>
            // 取最后一个空白分隔的 token 才是地址
            size_t e = line.size();
            while (e > 0 && (line[e - 1] == ' ' || line[e - 1] == '	' || line[e - 1] == '\r')) --e;
            size_t s = e;
            while (s > 0 && line[s - 1] != ' ' && line[s - 1] != '	') --s;
            std::string hostport = line.substr(s, e - s);
            if (hostport.find(':') == std::string::npos) continue;
            found = hostport;
            break;      // 多台设备时取第一台；用户可以自己改
        }

        std::lock_guard<std::mutex> lk(pairMtx_);
        if (!found.empty())
        {
            pairEndpoint_ = Widen(found);
            PerfLog("PAIR mDNS 找到配对地址 %s", found.c_str());
        }
        else
        {
            pairEndpoint_.clear();
            PerfLog("PAIR mDNS 没找到 _adb-tls-pairing（手机上要打开「使用配对码配对设备」）");
        }
    });
}

void AdbChannel::ConnectDirectAsync(const std::string& addr)
{
    if (addr.empty()) return;
    upgradeResult_ = 0;
    Post([this, addr]()
    {
        bool ok = ConnectWireless(addr, 6000);   // 失败时会顺手 disconnect，避免留 offline 幽灵
        Probe();

        // 连上以后把它记进已知列表，下次启动会自动重连
        if (ok)
        {
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            knownWireless_.insert(addr);
            identCache_.clear();
        }
        std::lock_guard<std::mutex> lk2(pairMtx_);
        upgradeStatus_ = ok ? L"已连接。"
                            : (L"连不上 " + Widen(addr) + L"：手机上的无线调试可能没开，或端口变了。");
        upgradeResult_ = ok ? 1 : 2;
    });
}

void AdbChannel::PairWirelessAsync(const std::string& addr, const std::string& code, const std::string& portHint)
{
    if (addr.empty() || code.empty()) return;
    if (pairBusy_.exchange(true))
    {
        // 以前这里直接 return，用户改了参数重试会毫无反应（以为还在配对）
        std::lock_guard<std::mutex> lk(pairMtx_);
        pairStatus_ = L"上一次配对还在进行中，请稍候几秒再试。";
        return;
    }
    pairCancel_ = false;
    pairResult_ = 0;
    { std::lock_guard<std::mutex> lk(pairMtx_); pairStatus_ = L"正在配对…"; }

    auto setStatus = [this](const std::wstring& t) {
        std::lock_guard<std::mutex> lk(pairMtx_);
        pairStatus_ = t;
    };

    Post([this, addr, code, portHint, setStatus]()
    {
        struct BusyGuard {
            std::atomic<bool>* f; ~BusyGuard() { *f = false; }
        } guard{ &pairBusy_ };

        // ① 配对
        std::string out = RunAdb("pair " + addr + " " + code, 20000);
        if (pairCancel_)
        {
            setStatus(L"已取消配对。");
            return;
        }
        std::string flat;
        for (char c : out) flat += (c == 10 || c == 13) ? '|' : c;
        PerfLog("PAIR adb pair %s -> [%s]", addr.c_str(), flat.c_str());

        if (out.find("Successfully paired") == std::string::npos)
        {
            pairResult_ = 2;      // UI 据此把按钮从「配对中…」恢复成可点的「重试配对」
            if (out.find("cannot connect") != std::string::npos || out.empty() ||
                out.find("protocol fault") != std::string::npos)
                setStatus(L"连不上配对端口。请确认：手机已开启「无线调试」、和电脑在同一 WiFi、地址和配对码是当前屏幕上显示的那一组。");
            else if (out.find("Wrong password") != std::string::npos ||
                     out.find("failed to authenticate") != std::string::npos)
                setStatus(L"配对码不对，或配对码已过期（每次打开配对界面都会变）。请重新输入。");
            else
            {
                // 兜底：把 adb 原文压成一行给用户，去掉内部用的 | 分隔符
                std::string clean;
                for (char c : flat) if (c != '|') clean += c;
                setStatus(L"配对失败：" + Widen(clean));
            }
            return;
        }

        // ② 配对成功 → 找「连接端口」（和配对端口不是同一个，也是随机的）
        std::string ip = addr.substr(0, addr.find(':'));
        std::string connectSerial;

        if (!portHint.empty())
        {
            std::string target = ip + ":" + portHint;
            // 必须看返回值：连不上时 adb 也会正常退出；
            // ConnectWireless 还会在失败时 disconnect，避免留 offline 幽灵条目。
            if (ConnectWireless(target, 6000)) connectSerial = target;
        }
        else
        {
            // 从 mDNS 里找 _adb-tls-connect
            for (int attempt = 0; attempt < 4 && connectSerial.empty(); ++attempt)
            {
                std::string md = RunAdb("mdns services", 5000);
                size_t pos = 0;
                while (pos < md.size())
                {
                    size_t nl = md.find(10, pos);
                    std::string line = md.substr(pos, (nl == std::string::npos) ? std::string::npos : nl - pos);
                    pos = (nl == std::string::npos) ? md.size() : nl + 1;
                    if (line.find("_adb-tls-connect") == std::string::npos) continue;
                    // 真实输出是 TAB 分隔，例如：
                    //   adb-XXXX	_adb-tls-connect._tcp	192.168.1.100:37123
                    // 不能找空格 —— 那样永远取不到地址（mDNS 分支会恒为空）。
                    // 取最后一个「空白分隔」的 token 才是 host:port。
                    size_t e2 = line.size();
                    while (e2 > 0 && (line[e2 - 1] == ' ' || line[e2 - 1] == '	' || line[e2 - 1] == '\r')) --e2;
                    size_t s2 = e2;
                    while (s2 > 0 && line[s2 - 1] != ' ' && line[s2 - 1] != '	') --s2;
                    std::string hostport = line.substr(s2, e2 - s2);
                    if (hostport.find(ip + ":") != 0) continue;
                    if (!ConnectWireless(hostport, 6000)) continue;
                    connectSerial = hostport;
                    break;
                }
                if (connectSerial.empty()) Sleep(800);
            }
        }

        // ③ 兜底：adb 可能自己就连上了（Android 11+ 常见），轮询 devices
        if (connectSerial.empty())
        {
            for (int attempt = 0; attempt < 6; ++attempt)
            {
                std::string devs = AdbServerDevices();
                if (devs.find(ip + ":") != std::string::npos) break;
                Sleep(800);
            }
        }

        Probe();

        // 唯一成功依据：adb devices 里真的出现了这个 ip:port
        if (AdbServerDevices().find(ip + ":") != std::string::npos)
        {
            // 记进已知无线列表：之后启动会直接重连
            std::string devs = AdbServerDevices();
            size_t p = devs.find(ip + ":");
            if (p != std::string::npos)
            {
                // serial 到空白/换行就结束
                size_t e = p;
                while (e < devs.size() && (unsigned char)devs[e] > 32) ++e;
                std::string serial = devs.substr(p, (e == std::string::npos) ? std::string::npos : e - p);
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                knownWireless_.insert(serial);
                identCache_.clear();
            }
            pairResult_ = 1;
            setStatus(L"配对成功，已连上。之后手机重启也不用再插线。");
        }
        else
        {
            // 这种情况按钮必须还是「配对」：状态文字让用户填端口后再点一次，
            // 如果这里标成成功，按钮会变成「完成」并关掉窗口，和文字自相矛盾。
            pairResult_ = 2;
            setStatus(L"配对已成功，但没找到连接端口。请在手机的「无线调试」页查看当前的 IP:端口，填到第三栏后点「配对」。");
        }
    });
}


#include "adb_bootstrap.inc"
