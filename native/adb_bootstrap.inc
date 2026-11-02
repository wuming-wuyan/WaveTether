// 蓝牙引导：USB 插入时自动 arm 无线 ADB；蓝牙连上后自动升级
// 由 adb.cpp #include 引入（在 AdbChannel 定义之后）

// 取手机 wlan0 的 IP
std::string AdbChannel::QueryWifiIp(const std::string& serial)
{
    std::string out = RunAdb("-s " + serial + " shell ip route", 4000);
    size_t p = out.find("src ");
    if (p == std::string::npos) return {};
    p += 4;
    size_t e = out.find_first_of(" \r\n", p);
    return out.substr(p, (e == std::string::npos) ? std::string::npos : e - p);
}

// 有线且已授权的设备（复用统一的解析）
static std::vector<std::string> ListWiredDevices(AdbChannel* self)
{
        std::string raw = AdbServerDevices();
    if (raw.empty()) raw = self->RawAdb("devices", 4000);   // 兜底
    return ParseDevices(raw, /*onlyWired=*/true, /*onlyAuthorized=*/true);
}

void AdbChannel::BootstrapTick()
{
    if (adbPath_.empty()) return;
    if (!bootGate_.Due(2000)) return;

    // 注意：不能用「wirelessIp_ 非空」当闸门——它会被持久化/自动采纳，
    // 几乎总是非空，结果 USB 引导（tcpip 5555）永远不执行。
    // 改成按设备记录「这台已经 arm 过」。
    if (state_ == AdbState::Running) return;

    // ① 已有无线设备在线（比如上次连接过、adb server 还记得）→ 直接采纳
    for (auto& ser : ParseDevices(AdbServerDevices(), /*onlyWired=*/false, /*onlyAuthorized=*/true))
    {
        // 只看无线。mDNS 名字也算无线（没有冒号但确实是无线的）
        bool mDNS = ser.find("_adb-tls-") != std::string::npos;
        if (ser.find(':') == std::string::npos && !mDNS) continue;
        if (mDNS) return;      // mDNS 名字里没有 IP，采纳它没有意义（也不需要再 arm）
        SetWirelessIp(ser.substr(0, ser.find(':')));
        // 只在这个值真的变了时记一行：否则每 2 秒刷一次日志，文件很快被撑大
        static std::string lastAdopted;
        if (lastAdopted != ser) { lastAdopted = ser; PerfLog("ADB Bootstrap: 采纳已在线的无线设备 %s", ser.c_str()); }
        return;
    }

    // ② 有 USB → 对「还没 arm 过」的设备执行 tcpip 5555 把无线打开
    // 用户可以在设置里关掉「自动开启无线 ADB」：关闭后只保留上面「采纳已在线设备」，
    // 不再主动把手机端口开到网络上。
    if (!autoArmWireless_) return;
    auto wired = ListWiredDevices(this);
    if (wired.empty())
    {
        // 临时诊断：没插线时这里直接返回，是「启动不连接」的一半原因
        static int idleCount = 0;
        if (++idleCount % 40 == 1)   // 每 80 秒记一次，别刷屏
            PerfLog("ADB Bootstrap: 没有 USB 设备，无法 arm（已第 %d 次空转）", idleCount);
        return;
    }

    // 跳过本次启动已经 arm 过的设备（插着线时每 2 秒会进来一次）
    std::string target;
    for (auto& ser : wired)
        if (!armedSerials_.count(ser)) { target = ser; break; }
    if (target.empty()) return;

    std::string ip = QueryWifiIp(target);
    if (ip.empty()) { lastError_ = "取不到设备 IP"; return; }

    if (GetEnvironmentVariableW(L"WT_TRACE", nullptr, 0) > 0)
    {
        FILE* lf=nullptr; _wfopen_s(&lf, LogPathIn(L"tcpip.log").c_str(), L"a");
        if(lf){ fprintf(lf,"TCPIP triggered for %s\n", target.c_str()); fclose(lf); }
    }
    std::string r = RunAdb("-s " + target + " tcpip 5555", 8000);
    if (r.find("5555") == std::string::npos && r.find("restarting") == std::string::npos)
    {
        lastError_ = "tcpip 5555 失败";
        return;
    }

    SetWirelessIp(ip);
    armedSerials_.insert(target);     // 这台已 arm，不再重复
    PerfLog("ADB Bootstrap: 已对 %s 执行 tcpip 5555，IP=%s", target.c_str(), ip.c_str());

    Sleep(1500);
    ConnectWireless(ip + ":5555");
}
