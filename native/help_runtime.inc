// 「运行时文件」帮助页：先给结论，再讲原因。
// 单文件版会把内嵌的 adb/scrcpy 解压到 Temp，用户最关心「能不能删」「会不会残留」。
//
// 注意：这里不用 GDI+ 的 MeasureString 算高度——它对中英混排的换行估算不可靠，
// 会导致多段文字重叠。改成自己按宽度断行，行高固定，布局完全可控。

// 按可用宽度把文本断成多行（支持显式 \n）
static std::vector<std::wstring> WrapByWidth(Graphics& g, const std::wstring& s, float w, float px)
{
    std::vector<std::wstring> out;
    std::wstring cur;

    for (size_t i = 0; i < s.size(); ++i)
    {
        wchar_t c = s[i];
        if (c == L'\n')
        {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        std::wstring probe = cur;
        probe += c;
        if (!cur.empty() && TextWidth(g, probe, px) > w)
        {
            out.push_back(cur);
            cur.assign(1, c);
        }
        else
        {
            cur.swap(probe);
        }
    }
    if (!cur.empty() || out.empty()) out.push_back(cur);
    return out;
}

// 返回内容总高度，供外层算滚动范围
static float DrawRuntimeHelp(Graphics& g, float x, float y, float w)
{
    const float kLine = S(18);     // 固定行高，保证可控
    float py = y;

    // 画一段：自动断行，返回占用的高度
    auto block = [&](const wchar_t* text, float px, COLORREF c, float gapAfter) -> float
    {
        auto lines = WrapByWidth(g, text, w, px);
        for (auto& ln : lines)
        {
            Text(g, ln, x, py, px, c);
            py += kLine;
        }
        py += gapAfter;
        return (float)lines.size() * kLine + gapAfter;
    };

    // 形态看「exe 里有没有内嵌资源」，不看临时目录里有没有释放好的文件：
    // 后者取决于用户是否正常启动过，干净机器上会把单文件版误判成蓝牙版。
    if (!::g.adb.HasEmbedded())
    {
        if (!::g.adb.HasAdb())
        {
            // 蓝牙版：连 tools 都没有，不能说「用 exe 旁边的 tools」
            block(L"一句话：本机是「蓝牙版」，没有 ADB 组件，所以不往临时文件夹写任何东西。",
                  S(13.f), ::g.th.Text, S(6));
            block(L"它只用蓝牙通道。想用 ADB（音质更好、延迟更低、可多台同时播），"
                  L"换成单文件版或带 tools 的便携版即可。",
                  S(12.5f), ::g.th.Ok, 0);
        }
        else
        {
            block(L"一句话：本机是「便携版」，不往临时文件夹写任何东西。",
                  S(13.f), ::g.th.Text, S(6));
            block(L"程序直接用 exe 旁边的 tools 文件夹，删掉整个文件夹就彻底干净，不留任何痕迹。",
                  S(12.5f), ::g.th.Ok, 0);
        }
        return py - y;
    }

    // 单文件版但这次没释放成功：先把原因说清楚，再讲常规说明
    if (::g.adb.EmbeddedRoot().empty() && !::g.adb.EmbeddedPending())
    {
        std::wstring why = ::g.adb.EmbedError();
        block(L"一句话：本机是「单文件版」，但这次没能把内嵌工具解压到临时文件夹。",
              S(13.f), ::g.th.Text, S(4));
        block(why.empty() ? L"具体原因见窗口顶部的红色提示。"
                          : (L"原因：" + why + L"。").c_str(),
              S(12.5f), ::g.th.Warn, S(6));
        block(L"常见处理：杀毒软件放行本程序 / 清理一下磁盘空间 / 关掉占用临时目录的程序后重新启动。",
              S(12.5f), ::g.th.TextMuted, 0);
        return py - y;
    }

    // ── 结论（放最前）──
    block(L"一句话：单文件版首次运行会把 adb / scrcpy 解压到临时文件夹（约 25 MB），之后每次启动直接复用。",
          S(13.f), ::g.th.Text, S(4));
    block(L"删掉它没有任何影响，下次启动会自动重新解压。",
          S(12.5f), ::g.th.Ok, S(14));

    // ── 实际位置 ──
    block(L"当前实际位置", S(12.f), ::g.th.TextMuted, S(2));
    {
        std::wstring root = ::g.adb.EmbeddedRoot();
        std::wstring line = root.empty() ? std::wstring(L"%TEMP%\\WaveTether\\rt-<哈希>\\") : root;
        block(line.c_str(), S(11.5f), ::g.th.AccentText, S(14));
    }

    // ── 为什么放 Temp ──
    block(L"为什么放在 Temp", S(12.f), ::g.th.TextMuted, S(2));
    block(L"Temp 就是「可以随时清掉的临时文件」，清它最放心。不在退出时删除，是因为留着下次启动更快（省约 1 秒）。",
          S(12.f), ::g.th.TextMuted, S(14));

    // ── 什么时候重新解压 ──
    block(L"什么时候会重新解压", S(12.f), ::g.th.TextMuted, S(2));
    block(L"· 你手动清理了 Temp\n· 程序更新了内嵌的 adb / scrcpy（目录名跟着内容变）\n· 换了一台电脑",
          S(12.f), ::g.th.TextMuted, S(14));

    // ── 会不会残留 ──
    block(L"会不会有残留", S(12.f), ::g.th.TextMuted, S(2));
    block(L"旧版本的目录会在下次启动时自动清理。程序被强制结束时目录会留着，但下次启动会直接复用，不额外占空间。",
          S(12.f), ::g.th.TextMuted, S(14));

    // ── 不想要残留 ──
    block(L"如果希望完全不写临时文件", S(12.f), ::g.th.TextMuted, S(2));
    block(L"用「便携版」：exe 旁边放一个 tools 文件夹，不往 Temp 写任何东西，删掉文件夹就彻底干净。",
          S(12.f), ::g.th.TextMuted, 0);

    return py - y;
}
