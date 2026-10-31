// SaveSettings 单独放这里，避免主文件里的转义地狱
// 由 main.cpp #include 引入
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
        for (auto& kv : ::g.devVolPref)
        {
            if (!first) s += ", ";
            first = false;
            char buf[32]; sprintf_s(buf, "%d", (int)(kv.second * 100.f + 0.5f));
            s += "\"" + JsonEscape(WideToUtf8(kv.first)) + "\": " + buf;
        }
    }
    s += "},\n";
    s += "  \"DevMute\": {";
    {
        bool first = true;
        for (auto& kv : ::g.devMutePref)
        {
            if (!first) s += ", ";
            first = false;
            s += "\"" + JsonEscape(WideToUtf8(kv.first)) + "\": " + (kv.second ? "true" : "false");
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
