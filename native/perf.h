// 统一诊断日志（PerfLog / PerfScope / PerfU8）
//
// 写 %APPDATA%\WaveTether\perf.log，每行以「系统运行毫秒数（GetTickCount64）」开头，
// 所以能直接看出事件之间的间隔。
//
// 文件生命周期：
//   · 每次启动重建（见 main.cpp 的 ResetPerfLogOnce）
//   · 单次运行内超过 2MB 会转存为 perf.log.old 再继续写
//   · 如果 perf.log 被别的程序独占（用户拿记事本打开看），退到 perf.log.1
//
// 用户反馈问题时让 Ta 点「打开日志」把这里的东西发过来。
//
// 两个必须保持的写法（都是踩过的坑）：
//   1. inline 而不是 static。static 在头文件里意味着「每个 .cpp 各一份」，
//      于是每个编译单元都有自己的锁 —— 多个 TU 同时写同一个文件却互不互斥。
//   2. 锁和路径都用「函数内 static 对象」，不要用 `if (!inited) {...}` 手工初始化。
//      C++11 的 magic static 保证只初始化一次且线程安全；手工写的
//      `if (!inited) { inited = true; InitializeCriticalSection(&cs); }`
//      有真实竞态：另一个线程可能看到 inited==true 时 cs 还没初始化。
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>

// 宽字符串转 UTF-8。
// 不能用 %ls：PerfLog 是 char 版 printf，%ls 会走 C locale 的宽转窄，
// 中文直接转失败 → 整行输出为空（踩过）。
inline std::string PerfU8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

namespace perfdetail
{
    inline std::wstring BuildPath()
    {
        wchar_t ap[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"APPDATA", ap, MAX_PATH) == 0) return std::wstring();
        std::wstring d = std::wstring(ap) + L"\\WaveTether";
        CreateDirectoryW(d.c_str(), nullptr);
        return d + L"\\perf.log";
    }

    struct LogState
    {
        CRITICAL_SECTION cs;
        int sinceCheck = 0;
        const std::wstring path;
        LogState() : path(BuildPath()) { InitializeCriticalSection(&cs); }
        ~LogState() { DeleteCriticalSection(&cs); }
    };

    // 持锁期间的 RAII 守卫：写日志里要做字符串拼接（可能 bad_alloc），
    // 手工 Enter/Leave 一旦中间抛异常就会永久占着锁 —— 之后所有线程写日志全卡死。
    struct CsGuard
    {
        CRITICAL_SECTION* c;
        explicit CsGuard(CRITICAL_SECTION* p) : c(p) { EnterCriticalSection(c); }
        ~CsGuard() { LeaveCriticalSection(c); }
    };

    inline bool WriteLine(const std::wstring& file, double ms, const char* body)
    {
        FILE* f = nullptr;
        _wfopen_s(&f, file.c_str(), L"a");
        if (!f) return false;
        fprintf(f, "%10.1f  %s\n", ms, body);
        fclose(f);
        return true;
    }
}

inline void PerfLog(const char* fmt, ...)
{
    static perfdetail::LogState st;          // magic static：只初始化一次，线程安全
    if (st.path.empty()) return;

    // 用 GetTickCount64：所有编译单元共用同一个时钟。
    double ms = (double)GetTickCount64();

    char body[1024];
    va_list a;
    va_start(a, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, a);
    va_end(a);

    perfdetail::CsGuard guard(&st.cs);

    // 轮转：每 128 行才查一次大小，避免每行都做一次文件系统调用。
    if (++st.sinceCheck >= 128)
    {
        st.sinceCheck = 0;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(st.path.c_str(), GetFileExInfoStandard, &fad))
        {
            ULONGLONG sz = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            if (sz > 2ull * 1024 * 1024)
            {
                // 转存一份上一段。失败也没关系：继续往原文件追加就是了，
                // 顶多是文件大一点，不能因为改名失败就把日志丢了。
                std::wstring old = st.path + L".old";
                MoveFileExW(st.path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        }
    }

    // 主文件被别的程序独占时（用户拿记事本打开看）退到 perf.log.1，
    // 否则日志会整条静默消失 —— 而「打开日志发给我」正是产品自己教的用法。
    if (!perfdetail::WriteLine(st.path, ms, body))
        perfdetail::WriteLine(st.path + L".1", ms, body);

}

// 简易作用域计时器：析构时若超过阈值就记一行
struct PerfScope
{
    const char* what;
    double limit;
    LARGE_INTEGER f, a;
    int extra;

    PerfScope(const char* w, double lim, int ex = 0) : what(w), limit(lim), extra(ex)
    {
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
    }
    ~PerfScope()
    {
        LARGE_INTEGER b;
        QueryPerformanceCounter(&b);
        double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
        if (ms >= limit) PerfLog("SLOW %-22s %7.1f ms  extra=%d", what, ms, extra);
    }
};
