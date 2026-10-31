// ---------------- JSON 小工具（手写，够用即可）----------------
// 三个前提，缺一就会静默丢数据（都踩过）：
//   1) 跳字符串必须认转义，否则值里的 \" 会被当成结束
//   2) 切对象/数组必须跳过字符串内容，否则值里的 } 会提前截断整个对象
//   3) 反转义必须和 JsonEscape 对称，否则 \n 会被读成字母 n
#pragma once
#include <string>
#include <vector>

// i 指向起始引号，返回结束引号之后的位置（未闭合则返回 size）
static size_t JsonSkipString(const std::string& j, size_t i)
{
    if (i >= j.size() || j[i] != '"') return i;
    ++i;
    while (i < j.size())
    {
        if (j[i] == '\\') { i += 2; continue; }
        if (j[i] == '"') return i + 1;
        ++i;
    }
    return j.size();
}

// 把 [start, end) 这段原始文本按 JSON 转义规则还原
static std::string JsonUnescape(const std::string& j, size_t start, size_t end)
{
    if (end > j.size()) end = j.size();
    std::string out;
    out.reserve(end > start ? end - start : 0);
    for (size_t i = start; i < end; ++i)
    {
        if (j[i] != '\\' || i + 1 >= end) { out += j[i]; continue; }
        char e = j[++i];
        switch (e)
        {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'u':
        {
            if (i + 4 >= end) { out += 'u'; break; }
            unsigned cp = 0; bool ok = true;
            for (int k = 1; k <= 4; ++k)
            {
                char h = j[i + k];
                cp <<= 4;
                if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                else { ok = false; break; }
            }
            if (!ok) { out += 'u'; break; }
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDFFF) out += "\xEF\xBF\xBD";        // 孤立代理项
            else if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
            else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
            break;
        }
        default: out += e; break;
        }
    }
    return out;
}

// 从 open（指向 openCh）开始找配对的 closeCh，返回内部文本。
// 字符串里的括号会被跳过，嵌套同种括号会累加深度。
static std::string JsonSlice(const std::string& j, size_t open, char openCh, char closeCh)
{
    if (open >= j.size() || j[open] != openCh) return {};
    int depth = 0;
    size_t i = open;
    while (i < j.size())
    {
        char c = j[i];
        if (c == '"') { i = JsonSkipString(j, i); continue; }
        if (c == openCh) ++depth;
        else if (c == closeCh)
        {
            --depth;
            if (depth == 0) return j.substr(open + 1, i - open - 1);
        }
        ++i;
    }
    return {};
}

// 解析 ["a","b"] 的内部文本（元素可含任意转义字符）
static std::vector<std::string> JsonParseStringArray(const std::string& body)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < body.size())
    {
        size_t q = body.find('"', i);
        if (q == std::string::npos) break;
        size_t e = JsonSkipString(body, q);
        if (e <= q + 1) { i = e; continue; }
        out.push_back(JsonUnescape(body, q + 1, e - 1));
        i = e;
    }
    return out;
}
