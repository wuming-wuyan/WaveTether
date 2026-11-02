// 开机自启：注册表只在这里读写。
// 单独拆出来是为了把「注册表访问」收拢到一处：以前消息处理函数直接写
// HKCU\...\Run，排查时要翻主文件；现在整个程序只有这个文件碰注册表。
//
// 由 main.cpp #include 引入（依赖 ::g 与 GetModuleFileNameW）。

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
