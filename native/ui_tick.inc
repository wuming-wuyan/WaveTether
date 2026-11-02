// 定时器逻辑（原 WM_TIMER 分支）。由 main.cpp #include 引入。
// 每个 TickXxx 对应原来的一段，顺序与原来一致；UiTick 只负责按顺序调用。

static void TickPersist()
{
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
}

static void TickBtUpgrade()
{
    // 蓝牙已连接 → 精确匹配对应的 ADB 设备并升级到 ADB 音频
    // 依据：设备自己的蓝牙名（settings secure bluetooth_name）与蓝牙名一致
    if (::g.bt.State() == LinkState::Connected && !::g.bt.DeviceName().empty())
    {
        if (::g.adb.UiDevices().empty()) ::g.adb.PostProbe([]{ ::g.adb.Probe(); });

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
            static PeriodicGate upgradeGate;
            if (upgradeGate.Due(5000))   // 5 秒试一次，避免频繁
            {
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
}

static void TickScanAdopt()
{
    // 蓝牙模式：周期性刷新设备列表，新配对/移除的设备能自动出现
    if (!::g.useAdb && ::g.bt.State() != LinkState::Connected && ::g.btScanGate.Due(60000))  // 60 秒一次（WinRT 枚举较重）
    {
        RefreshDevices();   // 后台枚举，不阻塞 UI
    }
    // 接管外部 scrcpy 不需要 4Hz：只在「自己一个会话都没跑」时才可能用到，
    // 而 FindExternalScrcpy 是全进程快照（7.8ms/次）。降到约 4 秒一次。
    if (::g.useAdb && ::g.adoptGate.Due(4000))
    {
        ::g.adb.Post([]{ ::g.adb.AdoptExternal(); });
    }
}

static void TickUpgradeResult()
{
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
}

static void TickPairUi()
{
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
}

static void TickIdentitySync()
{
    // 每 4 秒把在线 ADB 设备的蓝牙身份（名字+MAC）记进总列表
    {
        static PeriodicGate identGate;
        if (::g.useAdb && identGate.Due(4000))
        {
            bool ch = false;
            bool migrated = false;
            for (auto& d : ::g.adb.UiDevices())
            {
                if (d.state != "device") continue;
                if (::g.RememberAdbDevice(d.identity, Utf8ToWide(d.btName), d.btMac)) ch = true;

                // 老数据迁移：音量/静音以前按 serial 记，同一台设备会有多条。
                // 看到设备在线就顺手并到 identity 键上（只需做一次）。
                if (!d.identity.empty() && d.serial != d.identity)
                {
                    std::wstring oldKey = L"adb:" + Utf8ToWide(d.serial);
                    std::wstring newKey = AdbVolKey(d);
                    if (MoveKey(::g.devVolPref, oldKey, newKey)) ch = true;
                    if (MoveKey(::g.devMutePref, oldKey, newKey)) ch = true;

                    // 更早的版本按 ip:port 记键，加载时摘进 legacy*。设备现在上线了，
                    // 地址就是它，可以直接迁过去，不再静默丢掉。
                    if (MoveKey(::g.devVolPref, ::g.legacyVol, oldKey, newKey)) { ch = true; migrated = true; }
                    if (MoveKey(::g.devMutePref, ::g.legacyMute, oldKey, newKey)) { ch = true; migrated = true; }
                }
            }
            if (ch) ::g.btHistoryDirty = true;   // 复用同一个落盘标志
            if (migrated)
                ::g.SetNotice(L"已沿用这台设备升级前的音量设置", 0, 6000);
        }
    }
    if (::g.useAdb && ::g.probeGate.Due(6000))   // 约 6 秒探测一次（运行中列表变化少）
    {
        ::g.adb.PostProbe([]{ ::g.adb.Probe(); });   // 运行中也要刷新列表（Probe 内部保护运行状态）
    }
}

static void TickAudioSessions()
{
    // 音频会话枚举：每 1 秒一次（原来每 250ms 枚举两遍）
    static PeriodicGate sessGate;
    if (sessGate.Due(1000))
    {
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
                        e.key = AdbVolKey(d);       // identity 稳定，换链路不丢音量
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
                e.key = BtVolKey(nm, bmac);
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
}

static void TickDefaultOutput()
{
    // 默认输出设备：每 1 秒刷一次（原来每 250ms）
    static PeriodicGate defGate;
    if (defGate.Due(1000)) RefreshCurrentDefault();
}

static void TickBuffers()
{
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
}

static void TickBtDisconnect()
{
    PollBtScan();   // 取回后台枚举的蓝牙设备列表

    // 工作线程请求断开蓝牙 → 在这里（UI 线程）真正执行
    if (::g.btDisconnectRequest.exchange(false))
    {
        ::g.bt.Disconnect();
        ::g.SetNotice(L"音频被蓝牙占用，已自动断开蓝牙链路", 0, 10000);
    }
}

static void TickAutoStart()
{
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

        // 自动拉起每 3 秒最多一轮：StartDevice 是秒级 adb IO，
        // 按 250ms 抖动投递会在几秒内把队列堆满，之后只会被丢弃。
        static PeriodicGate autoStartGate;
        // 用去重后的列表：否则同一台设备的每个连接入口都会开一个 scrcpy
        // （手机同时开 tcpip 5555 + 无线调试时实测会开 4 个）
        auto merged = ::g.adb.UiUniqueDevices();
        if (autoStartGate.Due(3000))
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
}

static void TickBtSteal()
{
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
}

static void TickMuteHeal()
{
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
}

static void TickVolumeSync()
{
    if (!::g.volSelInit && ::g.volMode == 1 && !::g.devVols.empty() && !::g.SelectedEntry())
    {
        auto rows = CurrentRows();
        for (int i = 0; i < (int)rows.size(); i++)
        {
            std::wstring key = ::g.useAdb ? (L"adb:" + Utf8ToWide(rows[i].serial))
                                          : BtVolKey(rows[i].name, rows[i].btMac);
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
}

static void TickUiTail(HWND hwnd)
{
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


static void UiTick(HWND hwnd)
{
    ::g.bt.Tick();
    TickPersist();
    TickBtUpgrade();
    TickScanAdopt();
    TickUpgradeResult();
    TickPairUi();
    TickIdentitySync();
    TickAudioSessions();
    TickDefaultOutput();
    TickBuffers();
    TickBtDisconnect();
    TickAutoStart();
    TickBtSteal();
    TickMuteHeal();
    TickVolumeSync();
    TickUiTail(hwnd);
}
