// ---- 单文件版：内嵌工具释放 ----
// 资源由 gen_embed.py 生成：
//   1..N  RCDATA        每个文件（资源名是序号；内容可能是 XPRESS 压缩后的）
//   IDR_EMBED_MANIFEST  "序号<TAB>相对路径<TAB>原始大小<TAB>压缩后大小"
//                       （压缩后大小 = 0 表示未压缩，按原样写入）
//   IDR_EMBED_HASH      内嵌内容的 8 位哈希（决定释放目录名）
//
// 释放到 %TEMP%\WaveTether\rt-<哈希>\，退出时不删除：
// Temp 的语义就是「可以随时清掉的临时文件」，用户清掉后下次启动会重新释放。
// 换版本（哈希变化）时启动会修剪旧的 rt-* 目录。

// 递归删除目录（失败静默：文件可能正被占用）
static void RemoveDirTree(const std::wstring& dir)
{
    // 入口也要判：PruneRtDirs 会把 rt-* 根目录直接传进来，
    // 如果那个名字本身是个 junction，下面的 FindFirstFile(dir\*) 会跟着进到目标目录，
    // 然后逐个删掉目标里的文件。这种情况只删链接。
    {
        DWORD attr = GetFileAttributesW(dir.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
        {
            RemoveDirectoryW(dir.c_str());
            return;
        }
    }

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                // 重解析点（junction / 符号链接）绝对不能递归进去：
                // 目录里只要有人放过一个指向 C:\Windows 的 junction，
                // 这里就会把目标目录里的东西全删掉。只删链接本身。
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    RemoveDirectoryW(p.c_str());
                else
                    RemoveDirTree(p);
            }
            else
            {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

static std::wstring TempRoot()
{
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetTempPathW(MAX_PATH, buf);      // 已带结尾反斜杠
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring dir(buf);
    if (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    dir += L"\\WaveTether";
    return dir;
}

// 删掉 root 下所有 rt-* 目录，但保留 keep。
// 必须**无条件**调用（首次解压前也要），否则用户清了 Temp 又换过版本时，
// 旧目录会长期残留，每个约 15MB。
static void PruneRtDirs(const std::wstring& root, const std::wstring& keep)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"\\rt-*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring p = root + L"\\" + fd.cFileName;
        if (_wcsicmp(p.c_str(), keep.c_str()) != 0) RemoveDirTree(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// 从资源里读一小段文本（哈希用）
static std::string LoadEmbeddedText(HINSTANCE hInst, const wchar_t* name)
{
    HRSRC h = FindResourceW(hInst, name, RT_RCDATA);
    if (!h) return std::string();
    HGLOBAL d = LoadResource(hInst, h);
    if (!d) return std::string();
    const char* p = (const char*)LockResource(d);
    DWORD sz = SizeofResource(hInst, h);
    if (!p || sz == 0) return std::string();
    std::string s(p, sz);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

// 逐级创建目录
static void MakeDirs(const std::wstring& path)
{
    for (size_t i = 3; i <= path.size(); ++i)
    {
        if (i == path.size() || path[i] == L'\\')
        {
            std::wstring sub = path.substr(0, i);
            CreateDirectoryW(sub.c_str(), nullptr);
        }
    }
}

// XPRESS 解压：用 ntdll!RtlDecompressBuffer，Vista+ 恒在，不需要额外 DLL。
// （compressapi.dll 在精简版 Windows 上可能不存在，所以不用它。）
static bool XpressDecompress(const void* in, DWORD inSize, void* out, DWORD outSize)
{
    typedef LONG(NTAPI * RtlDecompressBuffer_t)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, PULONG);
    static RtlDecompressBuffer_t fn = nullptr;
    static bool resolved = false;
    if (!resolved)
    {
        resolved = true;
        HMODULE h = GetModuleHandleW(L"ntdll.dll");
        if (h) fn = (RtlDecompressBuffer_t)GetProcAddress(h, "RtlDecompressBuffer");
    }
    if (!fn) return false;

    ULONG final = 0;
    LONG st = fn(COMPRESSION_FORMAT_XPRESS, (PUCHAR)out, outSize, (PUCHAR)in, inSize, &final);
    return st == 0 && final == outSize;
}

struct EmbedEntry
{
    int id;
    std::wstring rel;
    DWORD origSize;
    DWORD compSize;      // 0 = 未压缩
};

// 释放失败时记下原因：调用方会当成「分离版」继续走，若不记录就只剩一句
// 「找不到 adb.exe」，把「磁盘满/文件被占用」误导成「包不完整」。
std::wstring AdbChannel::ExtractEmbeddedTools(bool prune)
{
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    // 没有内嵌资源 → 分离版，返回空让调用方去找 tools 目录
    HRSRC hMan = FindResourceW(hInst, L"IDR_EMBED_MANIFEST", RT_RCDATA);
    if (!hMan) return std::wstring();

    // 旧布局（%LOCALAPPDATA%\WaveTether\runtime）已废弃，顺手清掉，否则那 25MB 永不回收。
    // 自检模式（--selftest）是只读的：跑一次检查不该删用户磁盘上的任何东西。
    if (!probeOnly_)
    {
        wchar_t appdata[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH) > 0)
        {
            std::wstring old = std::wstring(appdata) + L"\\WaveTether\\runtime";
            if (GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES) RemoveDirTree(old);
        }
    }

    std::wstring root = TempRoot();
    if (root.empty()) { embedError_ = L"取不到临时目录路径"; return std::wstring(); }

    // 目录名用内容哈希：内嵌工具一变就换目录，不会沿用旧的
    std::string hash = LoadEmbeddedText(hInst, L"IDR_EMBED_HASH");
    if (hash.empty()) hash = "default";
    std::wstring base = root + L"\\rt-" + std::wstring(hash.begin(), hash.end());

    // 同名目录可能是别人预先放好的 junction / 符号链接：那样 FileExists 会通过，
    // 而后面的 RemoveDirTree / 写入都会顺着链接跑到别的地方。
    // 发现是重解析点就只删链接本身，并换一个不会冲突的名字。
    // 换完之后**还要再查一次**：-local 这个名字同样可能被人预先占成链接。
    // 自检模式不动它（删链接也是写操作）：直接由下面的检查报出来。
    for (int attempt = 0; attempt < 2 && !probeOnly_; ++attempt)
    {
        DWORD attr = GetFileAttributesW(base.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_REPARSE_POINT)) break;
        RemoveDirectoryW(base.c_str());          // 只删链接，不进目标目录
        base += L"-local";
        PerfLog("释放目录是重解析点，已移除链接并改用 %s", PerfU8(base).c_str());
    }
    {
        // 两次都不行就不硬来了：宁可用不了 ADB，也不能往一个来路不明的链接里写文件。
        DWORD attr = GetFileAttributesW(base.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
        {
            embedError_ = probeOnly_
                ? L"临时目录被占用成符号链接（自检不会删它；正常启动会移除链接后重建）"
                : L"临时目录被占用成符号链接，出于安全考虑拒绝写入";
            return std::wstring();
        }
    }

    // 解析清单
    HGLOBAL hManData = LoadResource(hInst, hMan);
    const char* man = hManData ? (const char*)LockResource(hManData) : nullptr;
    DWORD manSize = hManData ? SizeofResource(hInst, hMan) : 0;
    if (!man || manSize == 0) { embedError_ = L"内嵌清单损坏"; return std::wstring(); }

    std::vector<EmbedEntry> entries;
    {
        std::string manifest(man, manSize);
        size_t pos = 0;
        while (pos < manifest.size())
        {
            size_t nl = manifest.find('\n', pos);
            std::string line = manifest.substr(pos, (nl == std::string::npos) ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? manifest.size() : nl + 1;

            // 字段：序号 \t 相对路径 \t 原始大小 \t 压缩后大小
            std::vector<std::string> f;
            size_t sp = 0;
            while (true)
            {
                size_t t = line.find('\t', sp);
                f.push_back(line.substr(sp, (t == std::string::npos) ? std::string::npos : t - sp));
                if (t == std::string::npos) break;
                sp = t + 1;
            }
            if (f.size() < 4) continue;
            for (auto& x : f) while (!x.empty() && (x.back() == '\r' || x.back() == ' ')) x.pop_back();

            int id = atoi(f[0].c_str());
            if (id <= 0 || f[1].empty()) continue;

            std::wstring relW(f[1].begin(), f[1].end());
            for (auto& c : relW) if (c == L'/') c = L'\\';

            EmbedEntry e;
            e.id = id;
            e.rel = relW;
            e.origSize = (DWORD)strtoul(f[2].c_str(), nullptr, 10);
            e.compSize = (DWORD)strtoul(f[3].c_str(), nullptr, 10);
            if (e.origSize == 0 || e.origSize > 512u * 1024 * 1024) continue;
            entries.push_back(e);
        }
    }
    if (entries.empty()) { embedError_ = L"内嵌清单为空"; return std::wstring(); }

    // 探针路径从清单里推出来，不写死版本号：scrcpy 升级后版本目录名会变，
    // 写死会导致探针永远 miss，每次启动都重新解压（甚至因文件被占用而失败）。
    static const std::wstring kScrcpyTail = L"\\scrcpy.exe";
    bool hasScrcpy = false;
    for (auto& e : entries)
        if (e.rel.size() > kScrcpyTail.size() &&
            _wcsicmp(e.rel.c_str() + e.rel.size() - kScrcpyTail.size(), kScrcpyTail.c_str()) == 0)
            hasScrcpy = true;
    if (!hasScrcpy) { embedError_ = L"内嵌清单里没有 scrcpy.exe"; return std::wstring(); }

    // 无论复用还是首次释放，都先修剪别的 rt-*
    if (prune) PruneRtDirs(root, base);

    // 完整性检查：清单里每一项都得在，不能只看 scrcpy.exe。
    // 杀软隔离某个 dll / 用户手删 / 释放到一半被强杀，都会留下「看着完整」的目录，
    // 结果 scrcpy 启动即退，界面只报「scrcpy 启动后立即退出」，用户无从下手。
    {
        bool complete = true;
        std::wstring missing;        // 第一处不完整的相对路径（自检时用来报准确原因）
        for (auto& e : entries)
        {
            std::wstring f = base + L"\\" + e.rel;
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fad))
            {
                complete = false;
                missing = e.rel;
                break;
            }
            ULONGLONG sz = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            // 光看「文件在不在」不够：截断、被换成同名小文件、被换成链接都算不可信。
            // 这里至少卡住大小与重解析点（逐字节哈希暂未做，代价与收益见 README）。
            if (sz != e.origSize || (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            {
                complete = false;
                missing = e.rel;
                break;
            }
        }
        if (complete) return base;
        // 自检模式（--selftest）：绝不删、绝不写。
        // 否则「程序正在跑的时候执行一次 --selftest」会把运行中的运行时目录整个抹掉重写，
        // 正在播的 scrcpy/adb 直接被抽走 —— 这比「多写 26MB」严重得多。
        //
        // 但不写盘不等于不检查：干净机器上运行时目录本来就不存在，报 FAIL 会让
        // 用户和 CI 都以为包坏了。这里逐项确认「真释放时能成功」——资源在、大小对。
        if (probeOnly_)
        {
            for (auto& e : entries)
            {
                HRSRC hFile = FindResourceW(hInst, MAKEINTRESOURCEW(e.id), RT_RCDATA);
                DWORD size = 0;
                if (hFile)
                {
                    HGLOBAL hData = LoadResource(hInst, hFile);
                    if (hData) size = SizeofResource(hInst, hFile);
                }
                DWORD want = e.compSize ? e.compSize : e.origSize;
                if (!hFile || size != want)
                {
                    embedError_ = L"内嵌资源缺失或损坏（" + e.rel + L"）";
                    embedPending_ = false;
                    return std::wstring();
                }
            }
            embedError_.clear();
            embedPending_ = true;
            // 目录已经躺在磁盘上（只是不完整）：自检不能只说「尚未写盘」，
            // 那台机器正是需要被诊断的机器。正常启动会自动重建，所以只提示。
            if (GetFileAttributesW(base.c_str()) != INVALID_FILE_ATTRIBUTES)
                embedStale_ = missing;
            return std::wstring();
        }
        RemoveDirTree(base);      // 不完整就整个重来
    }

    // ---- 首次释放 ----
    std::vector<BYTE> plain;
    for (auto& e : entries)
    {
        HRSRC hFile = FindResourceW(hInst, MAKEINTRESOURCEW(e.id), RT_RCDATA);
        if (!hFile) { embedError_ = L"内嵌资源缺失（序号 " + std::to_wstring(e.id) + L"）"; break; }
        HGLOBAL hData = LoadResource(hInst, hFile);
        const void* data = hData ? LockResource(hData) : nullptr;
        DWORD size = hData ? SizeofResource(hInst, hFile) : 0;
        if (!data || size == 0) { embedError_ = L"内嵌资源为空（序号 " + std::to_wstring(e.id) + L"）"; break; }

        const void* writePtr = data;
        DWORD writeSize = size;
        if (e.compSize != 0)
        {
            if (size != e.compSize)
            {
                embedError_ = L"内嵌数据损坏（" + e.rel + L"）";
                break;
            }
            plain.resize(e.origSize);
            if (!XpressDecompress(data, size, plain.data(), e.origSize))
            {
                embedError_ = L"解压失败（" + e.rel + L"）";
                break;
            }
            writePtr = plain.data();
            writeSize = e.origSize;
        }
        else if (size != e.origSize)
        {
            embedError_ = L"内嵌数据长度不符（" + e.rel + L"）";
            break;
        }

        std::wstring outPath = base + L"\\" + e.rel;
        size_t sep = outPath.find_last_of(L'\\');
        if (sep != std::wstring::npos) MakeDirs(outPath.substr(0, sep));

        HANDLE hf = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf == INVALID_HANDLE_VALUE)
        {
            embedError_ = L"无法写入 " + e.rel + L"（错误码 " + std::to_wstring(GetLastError()) + L"）";
            break;
        }
        DWORD written = 0;
        WriteFile(hf, writePtr, writeSize, &written, nullptr);
        CloseHandle(hf);
        if (written != writeSize)
        {
            embedError_ = L"写入不完整 " + e.rel + L"（磁盘空间不足？）";
            break;
        }
    }

    if (!embedError_.empty())
    {
        // 半成品目录留着会让下次启动误判「已释放」，直接清掉重来
        RemoveDirTree(base);
        return std::wstring();
    }
    return base;
}
