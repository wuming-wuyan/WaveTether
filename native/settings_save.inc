// SaveSettings 单独放这里，避免主文件里的转义地狱
// 由 main.cpp #include 引入
static int VolPercent(float v) { return (int)(v * 100.f + 0.5f); }

static void AppendVolEntry(std::string& s, bool& first, const std::wstring& k, float v)
{
    if (!first) s += ", ";
    first = false;
    char buf[32]; sprintf_s(buf, "%d", VolPercent(v));
    s += "\"" + JsonEscape(WideToUtf8(k)) + "\": " + buf;
}

static void AppendMuteEntry(std::string& s, bool& first, const std::wstring& k, bool m)
{
    if (!first) s += ", ";
    first = false;
    s += "\"" + JsonEscape(WideToUtf8(k)) + "\": " + (m ? "true" : "false");
}

static void SaveSettings()
{
    EnsureSettingsPath();
    std::string s = "{\n";
    s += "  \"Theme\": \"" + std::string(::g.themeLight ? "light" : "dark") + "\",\n";
    s += "  \"OutputMode\": \"" + std::string(::g.fixedMode ? "fixed" : "follow") + "\",\n";
    s += "  \"RouteTarget\": \"" + JsonEscape(PerfU8(::g.routeTarget)) + "\",\n";
    s += "  \"RoutePrev\": \"" + JsonEscape(PerfU8(::g.routePrev)) + "\",\n";
    s += "  \"RouteDirty\": " + std::string(::g.routeDirty ? "true" : "false") + ",\n";
    s += "  \"PhoneVolume\": " + std::to_string((int)(::g.phoneVol * 100.f + 0.5f)) + ",\n";
    s += "  \"PhoneMuted\": " + std::string(::g.phoneMute ? "true" : "false") + ",\n";
    s += "  \"AutoStart\": " + std::string(::g.autoStart ? "true" : "false") + ",\n";
    s += "  \"AutoConnect\": " + std::string(::g.autoConnect ? "true" : "false") + ",\n";
    s += "  \"LanScan\": " + std::string(::g.adb.LanScan() ? "true" : "false") + ",\n";
    s += "  \"AutoAdapt\": " + std::string(::g.adb.AutoAdapt() ? "true" : "false") + ",\n";
    s += "  \"IdleReset\": " + std::string(::g.idleReset ? "true" : "false") + ",\n";
    s += "  \"AutoUpgrade\": " + std::string(::g.autoUpgrade ? "true" : "false") + ",\n";
    s += "  \"AutoReconnect\": " + std::string(::g.autoReconnect ? "true" : "false") + ",\n";
    s += "  \"AutoArmWireless\": " + std::string(::g.autoArmWireless ? "true" : "false") + ",\n";
    s += "  \"DualChannel\": " + std::string(::g.adb.DualChannel() ? "true" : "false") + ",\n";
    s += "  \"AutoDual\": " + std::string(::g.adb.AutoDual() ? "true" : "false") + ",\n";
    s += "  \"SetupDone\": " + std::string(::g.setupDone ? "true" : "false") + ",\n";
    s += "  \"SilentStart\": " + std::string(::g.silentStart ? "true" : "false") + ",\n";
    s += "  \"BufGear\": " + std::to_string(::g.adb.BufferGear()) + ",\n";
    s += "  \"AutoDevices\": [";
    {
        bool first = true;
        for (auto& k : ::g.autoDevices)
        {
            if (!first) s += ", ";
            first = false;
            s += "\"" + JsonEscape(k) + "\"";
        }
    }
    s += "],\n";
    s += "  \"WantedDevices\": [";
    {
        bool first = true;
        for (auto& k : ::g.adb.UiWantedSerials())
        {
            // 只存稳定的 ip:5555 形式：Android 11+ 的随机端口下次重启就失效，存了是废数据
            size_t c = k.rfind(':');
            if (c == std::string::npos || k.substr(c + 1) != "5555") continue;
            if (!first) s += ", ";
            first = false;
            s += "\"" + JsonEscape(k) + "\"";
        }
    }
    s += "],\n";
    s += "  \"KnownWireless\": [";
    {
        bool first = true;
        for (auto& k : ::g.adb.UiKnownWireless())
        {
            if (!first) s += ", ";
            first = false;
            s += "\"" + JsonEscape(k) + "\"";
        }
    }
    s += "],\n";
    s += "  \"BufPref\": {";
    {
        bool first = true;
        for (auto& kv : ::g.adb.UiBufferPrefs())
        {
            if (!first) s += ", ";
            first = false;
            s += "\"" + JsonEscape(kv.first) + "\": " + std::to_string(kv.second);
        }
    }
    s += "},\n";
    s += "  \"CloseAction\": \"" + JsonEscape(WideToUtf8(::g.closeAction)) + "\",\n";
    s += "  \"MinimizeAction\": \"" + JsonEscape(WideToUtf8(::g.minAction)) + "\",\n";
    s += "  \"LastDeviceId\": \"" + JsonEscape(WideToUtf8(::g.lastDeviceId)) + "\",\n";
    s += "  \"LastDeviceName\": \"" + JsonEscape(WideToUtf8(::g.lastDeviceName)) + "\",\n";
    s += "  \"WirelessIp\": \"" + ::g.adb.WirelessIp() + "\",\n";
    s += "  \"VolMode\": " + std::to_string(::g.volMode) + ",\n";
    // 蓝牙设备历史（MAC → 当前名字）。改名时按 MAC 覆盖，旧名字不会残留。
    s += "  \"BtDeleted\": [";
    {
        bool first = true;
        for (auto& k : ::g.btDeleted)
        {
            if (!first) s += ", ";
            first = false;
            s += "\"" + JsonEscape(k) + "\"";
        }
    }
    s += "],\n";
    s += "  \"BtMacs\": {";
    {
        bool first = true;
        for (auto& kv : ::g.btMacs)
        {
            if (!first) s += ", ";
            first = false;
            s += "\"" + JsonEscape(WideToUtf8(kv.first)) + "\": \"" + JsonEscape(WideToUtf8(kv.second)) + "\"";
        }
    }
    s += "},\n";
    s += "  \"BtHistory\": {";
    {
        bool first = true;
        for (auto& kv : ::g.btHistory)
        {
            if (!first) s += ", ";
            first = false;
            s += "\"" + JsonEscape(kv.first) + "\": \"" + JsonEscape(WideToUtf8(kv.second)) + "\"";
        }
    }
    s += "},\n";
    s += "  \"DevVol\": {";
    {
        bool first = true;
        for (auto& kv : ::g.devVolPref) AppendVolEntry(s, first, kv.first, kv.second);
        // 还没等到设备上线的旧键也写回去，否则「升级后先开程序、后连手机」就丢了。
        // 上限 16 条：它们是候选，不是当前设置，别让换了 IP 的历史无限堆积。
        int legacy = 0, dropped = 0;
        for (auto& kv : ::g.legacyVol)
        {
            if (::g.devVolPref.count(kv.first)) continue;
            if (legacy >= 16) { ++dropped; continue; }
            ++legacy;
            AppendVolEntry(s, first, kv.first, kv.second);
        }
        if (dropped)
        {
            static bool logged = false;
            if (!logged) { logged = true; PerfLog("旧音量键超过 16 条，本次丢弃 %d 条", dropped); }
        }
    }
    s += "},\n";
    s += "  \"DevMute\": {";
    {
        bool first = true;
        for (auto& kv : ::g.devMutePref) AppendMuteEntry(s, first, kv.first, kv.second);
        int legacy = 0, dropped = 0;
        for (auto& kv : ::g.legacyMute)
        {
            if (::g.devMutePref.count(kv.first)) continue;
            if (legacy >= 16) { ++dropped; continue; }
            ++legacy;
            AppendMuteEntry(s, first, kv.first, kv.second);
        }
        if (dropped)
        {
            static bool logged = false;
            if (!logged) { logged = true; PerfLog("旧静音键超过 16 条，本次丢弃 %d 条", dropped); }
        }
    }
    s += "},\n";
    s += "  \"BtNames\": {";
    {
        bool first = true;
        for (auto& kv : ::g.btNames)
        {
            if (!first) s += ", ";
            first = false;
            s += "\"" + JsonEscape(WideToUtf8(kv.first)) + "\": \"" + JsonEscape(WideToUtf8(kv.second)) + "\"";
        }
    }
    s += "},\n";
    s += "  \"OutputPriority\": [";
    for (size_t i = 0; i < ::g.prio.size(); i++)
    {
        if (i) s += ", ";
        s += "\"" + JsonEscape(WideToUtf8(::g.prio[i])) + "\"";
    }
    s += "]\n}\n";
    WriteAll(::g.settingsPath, s);
}
