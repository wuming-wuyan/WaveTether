#pragma once
// ---------------------------------------------------------------------------
// 纯函数（可单元测试的那部分）
//
// 这个文件存在的唯一理由：让「容易被边界值影响的纯逻辑」能被自动化测试。
// 本程序九成价值在「真手机 + 真 WiFi + 真音频设备」，那部分测不了；
// 这里集中覆盖播放器状态、设置文本、音量和挡位的边界判断：
//
//   HasPlayingState   —— 长度写死 32（实际 34）→ 恒假，静音自愈整段成了死代码
//   SwapBoolAt        —— std::swap 用在 vector<bool> 代理上 → 勾选静默翻转
//   PickSettingsText  —— settings.json 被截断时识别不出 → 配置全丢
//   ClampPercent      —— 脏值静默变 0 → 音量变 0，用户以为程序坏了
//   ClampGear         —— 超界挡位被当成 0（最激进、最易卡顿）
//   LooksLikeJson     —— 上面两条的判定基础
//
// 改这里的任何函数，请同步跑 tools\ci.bat（内含 test_pure）。
// ---------------------------------------------------------------------------
#include <string>
#include <vector>

// dumpsys media_session 的输出里是否有「正在播放」。
// 注意：模式串长度必须由编译器算（sizeof - 1），不能再写死数字。
inline bool HasPlayingState(const std::string& out)
{
    static const char kPlaying[] = "state=PlaybackState {state=PLAYING";
    const size_t kLen = sizeof(kPlaying) - 1;      // 34
    size_t pos = 0;
    while ((pos = out.find("state=PlaybackState", pos)) != std::string::npos)
    {
        if (out.compare(pos, kLen, kPlaying) == 0) return true;
        // 只前进 1：两个 state=PlaybackState 相邻（相隔 < 34）时，
        // 按 kLen 前进会直接跳过第二个，导致「正在播放」漏判。
        pos += 1;
    }
    return false;
}

// 粗判一段文本是不是「完整的 JSON 对象」（首尾分别是 { 和 }）。
// 用途：区分「文件不存在」和「文件被截断/写坏」——后者绝不能拿去覆盖 .bak。
inline bool LooksLikeJson(const std::string& s)
{
    size_t i = 0, j = s.size();

    // UTF-8 BOM：用户用记事本 / PowerShell「另存为 UTF-8」很容易带上。
    // 不跳过的话首字节是 0xEF，会被判成「文件损坏」→ 回退 .bak 甚至按默认值启动，
    // 用户手改的配置就白改了。
    if (j >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        i = 3;

    while (i < j && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    while (j > i && (s[j - 1] == ' ' || s[j - 1] == '\t' || s[j - 1] == '\r' || s[j - 1] == '\n')) --j;
    if (j <= i) return false;
    if (s[i] != '{' || s[j - 1] != '}') return false;

    // 只看首尾大括号不够：截断在 {"a":{"b":1} 这种位置时首尾正好是 { }，
    // 会被当成完整文件，于是后面的键静默丢失、也不走 .bak。
    // 这里做一次括号配平（跳过字符串内的括号和转义）。
    int depth = 0;
    bool inStr = false;
    for (size_t k = i; k < j; ++k)
    {
        char c = s[k];
        if (inStr)
        {
            if (c == '\\') { ++k; continue; }
            if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == '{') ++depth;
        else if (c == '}')
        {
            --depth;
            if (depth == 0) return (k == j - 1);   // 闭合后必须就是结尾
        }
    }
    return false;
}

// 主文件不可用时回退到 .bak；两个都不可用返回空串（调用方按默认值启动）。
inline std::string PickSettingsText(const std::string& mainText, const std::string& bakText)
{
    if (LooksLikeJson(mainText)) return mainText;
    if (LooksLikeJson(bakText)) return bakText;
    return std::string();
}

// 音量百分比钳制：NaN / 负数 → 默认满音量（不能当 0，否则「静音」看起来像 bug）；
// 超过 100 → 100。
inline double ClampPercent(double v, double def = 100.0)
{
    if (!(v >= 0.0)) return def;          // 同时挡掉 NaN
    if (v > 100.0) return 100.0;
    return v;
}

// 缓冲挡位：只有 0..3（保守/标准/积极/激进）。
inline int ClampGear(int g)
{
    if (g < 0) return 0;
    if (g > 3) return 3;
    return g;
}

// 每台设备学到的缓冲毫秒数（不是挡位！挡位是 ClampGear）。
// 下限 10ms：程序自己最小的挡位是 30ms，再小必然持续欠载；
// 上限 2000ms：再大延迟就完全不能用了。脏值（手改/旧版本/atoi 溢出）走默认。
inline int ClampBufferMs(int ms, int def = 220)
{
    if (ms <= 0) return def;
    if (ms < 10) return 10;
    if (ms > 2000) return 2000;
    return ms;
}

// 交换 std::vector<bool> 的两个元素。
// 必须用 bool 中转：vector<bool> 的元素是代理对象，std::swap 会「先覆盖再读」，
// 实测 {1,0} → {0,0}、{0,1} → {1,1}。
inline void SwapBoolAt(std::vector<bool>& v, size_t a, size_t b)
{
    if (a == b || a >= v.size() || b >= v.size()) return;
    bool t = v[a];
    v[a] = v[b];
    v[b] = t;
}
