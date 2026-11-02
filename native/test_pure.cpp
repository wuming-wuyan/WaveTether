// 纯函数单元测试。
//
// 用例围绕关键边界行为，覆盖解析、状态判定、脏值钳制和 JSON 截断。
//   1. HasPlayingState  长度写死 32（实际 34）→ 恒假，静音自愈整段成死代码
//   2. SwapBoolAt       std::swap 用在 vector<bool> 上 → 勾选静默翻转
//   3. PickSettingsText settings.json 截断识别不出 → 配置全丢
//   4. ClampPercent     脏值静默变 0 → 音量变 0
//   5. ClampGear        超界挡位被当成 0（最激进）→ 用户选的「保守」被静默降级
//   6. LooksLikeJson    上面两条的判定基础
//
// 跑法：native\obj\test_pure.exe（由 tools\ci.bat 调用）。退出码 0 = 全过。
#include "pure.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) ++g_pass;                                                \
        else { ++g_fail; printf("  FAIL  line %d:  %s\n", __LINE__, #cond); } \
    } while (0)

static void TestHasPlayingState()
{
    printf("[1] HasPlayingState（长度写死 32 vs 实际 34）\n");
    CHECK(HasPlayingState("  state=PlaybackState {state=PLAYING(1) id=abc}"));
    CHECK(HasPlayingState("state=PlaybackState {state=PLAYING"));
    CHECK(!HasPlayingState(""));
    CHECK(!HasPlayingState("state=PlaybackState {state=PAUSED}"));
    CHECK(!HasPlayingState("state=PlaybackState"));            // 只有前缀
    CHECK(!HasPlayingState("state=PlaybackState {state=PLAYIN"));  // 被截断
    CHECK(HasPlayingState("state=PlaybackState {state=STOPPED}\n"
                          "state=PlaybackState {state=PLAYING"));
}

static void TestSwapBoolAt()
{
    printf("[2] SwapBoolAt（std::swap 在 vector<bool> 上会静默算错）\n");
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b)
        {
            std::vector<bool> v{ a != 0, b != 0 };
            SwapBoolAt(v, 0, 1);
            CHECK((bool)v[0] == (b != 0));
            CHECK((bool)v[1] == (a != 0));
        }

    std::vector<bool> v{ true, false, false };
    SwapBoolAt(v, 0, 2);
    CHECK(!v[0] && !v[1] && v[2]);

    std::vector<bool> one{ true };
    SwapBoolAt(one, 0, 5);                 // 越界不应崩
    SwapBoolAt(one, 7, 9);
    SwapBoolAt(one, 0, 0);
    CHECK(one.size() == 1 && one[0]);
}

static void TestPickSettingsText()
{
    printf("[3] PickSettingsText（settings.json 截断 → 回退 .bak）\n");
    const std::string good = "{\"a\":1}";
    const std::string bad  = "{\"a\":1";
    CHECK(PickSettingsText(good, bad) == good);    // 主文件好 → 用主文件
    CHECK(PickSettingsText(bad, good) == good);    // 主文件坏 → 用备份
    CHECK(PickSettingsText("", good) == good);     // 主文件不存在 → 用备份
    CHECK(PickSettingsText(bad, bad).empty());     // 都坏 → 空（按默认值启动）
    CHECK(PickSettingsText("", "").empty());
}

static void TestClampPercent()
{
    printf("[4] ClampPercent（脏值不能静默变成 0）\n");
    CHECK(ClampPercent(0.0) == 0.0);
    CHECK(ClampPercent(50.0) == 50.0);
    CHECK(ClampPercent(100.0) == 100.0);
    CHECK(ClampPercent(150.0) == 100.0);
    CHECK(ClampPercent(-5.0) == 100.0);            // 负数 → 满音量，不是 0
    CHECK(ClampPercent(std::nan("")) == 100.0);    // NaN → 满音量
    CHECK(ClampPercent(std::nan(""), 42.0) == 42.0);
}

static void TestClampGear()
{
    printf("[5] ClampGear（超界挡位不能变成最激进）\n");
    CHECK(ClampGear(0) == 0);
    CHECK(ClampGear(1) == 1);
    CHECK(ClampGear(3) == 3);
    CHECK(ClampGear(-1) == 0);
    CHECK(ClampGear(4) == 3);
    CHECK(ClampGear(999) == 3);
}

static void TestLooksLikeJson()
{
    printf("[6] LooksLikeJson（边界 / BOM / 截断）\n");
    CHECK(LooksLikeJson("{}"));
    CHECK(LooksLikeJson("  {\"a\":1}\r\n"));
    CHECK(LooksLikeJson("{\"a\":{\"b\":1}}"));
    CHECK(!LooksLikeJson(""));
    CHECK(!LooksLikeJson("   "));
    CHECK(!LooksLikeJson("{\"a\":1"));           // 截断
    CHECK(!LooksLikeJson("{\"a\":{\"b\":1}"));   // 截断但首尾仍是 { } —— 必须靠括号配平拦住
    CHECK(!LooksLikeJson("{\"a\":1}} "));        // 多一个 }
    CHECK(!LooksLikeJson("[1,2]"));               // 数组不算
    CHECK(!LooksLikeJson("null"));
    CHECK(!LooksLikeJson("}"));
    CHECK(!LooksLikeJson("{"));

    // UTF-8 BOM：记事本/PowerShell 另存为 UTF-8 会带，不能判成损坏
    CHECK(LooksLikeJson("\xEF\xBB\xBF{\"a\":1}"));
    CHECK(LooksLikeJson("\xEF\xBB\xBF  {}"));
    CHECK(!LooksLikeJson("\xEF\xBB\xBF"));         // 只有 BOM

    // 字符串里的大括号不能参与配平
    CHECK(LooksLikeJson("{\"k\":\"}\"}"));
    CHECK(LooksLikeJson("{\"k\":\"{\"}"));
    CHECK(!LooksLikeJson("{\"k\":\"}"));        // 字符串未闭合
}

static void TestHasPlayingAdjacent()
{
    printf("[7] HasPlayingState 相邻匹配（不能跳着扫）\n");
    // 两个 state=PlaybackState 相隔 < 34：按模式串长度前进会直接跳过第二个
    CHECK(HasPlayingState("state=PlaybackState state=PlaybackState {state=PLAYING"));
    CHECK(HasPlayingState("state=PlaybackStateX state=PlaybackState {state=PLAYING"));
    CHECK(!HasPlayingState("state=PlaybackState state=PlaybackState {state=PAUSED"));
}

static void TestClampBufferMs()
{
    printf("[8] ClampBufferMs（每设备学到的缓冲是毫秒，不是挡位）\n");
    CHECK(ClampBufferMs(220) == 220);
    CHECK(ClampBufferMs(30) == 30);
    CHECK(ClampBufferMs(2000) == 2000);
    CHECK(ClampBufferMs(5000) == 2000);
    CHECK(ClampBufferMs(3) == 10);          // 比程序最小挡位还小 → 抬到 10
    CHECK(ClampBufferMs(0) == 220);         // 脏值 → 默认
    CHECK(ClampBufferMs(-5) == 220);
    CHECK(ClampBufferMs(1) == 10);
}

int main()
{
    TestHasPlayingState();
    TestSwapBoolAt();
    TestPickSettingsText();
    TestClampPercent();
    TestClampGear();
    TestLooksLikeJson();
    TestHasPlayingAdjacent();
    TestClampBufferMs();

    printf("\n通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
