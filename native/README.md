# native/ — WaveTether 原生实现

Win32 + GDI+ 手绘界面，无外部依赖，编译产物是免安装绿色版。

## 构建

```bat
build.bat          :: 便携版：WaveTether.exe（约 671 KB）+ tools\ 目录
build_single.bat   :: 单文件版：WaveTether-single.exe（约 14 MB，内嵌全部工具）
```

两个脚本都先 `call env.bat` 准备编译环境。

`env.bat` 用 `vswhere` 定位 Visual Studio（找不到再退回常见安装路径），
SDK 头文件路径取自 vcvars 设置的 `%WindowsSdkDir%` / `%WindowsSDKVersion%`，
**不硬编码任何版本号**。

> 踩坑记录：cmd.exe 解析 `.bat` 用的是 OEM 代码页，所以脚本里不写中文；
> 另外任何展开后含 `(x86)` 的路径都不能放进 `for (...)` 或 `( ... )` 块里，
> 那个 `)` 会提前闭合块，报出莫名其妙的 `\Windows was unexpected at this time`。

构建中间产物统一进 `native\obj\`，不散落在源码目录。

## 单文件版的内嵌资源

`gen_embed.py` 从 `tools\scrcpy\scrcpy-win64-*\` 里挑出 11 个运行必需的文件
（外加 5 个许可/第三方声明文件，共 16 个），
用 Windows 自带的 XPRESS 压缩（`ntdll!RtlCompressBuffer`，无需第三方库），
写成 RCDATA 资源：

| 资源 | 内容 |
|---|---|
| `1`..`16` | 每个文件一份（压缩后；压不小的按原样存） |
| `IDR_EMBED_MANIFEST` | `序号<TAB>相对路径<TAB>原始大小<TAB>压缩后大小` |
| `IDR_EMBED_HASH` | 内嵌内容的 SHA-1 前 8 位，决定运行时目录名 |

实测 25.0 MB → 12.7 MB（51%）。`gen_embed.py` 会在打包时做一次压缩往返自检，
压坏了在构建阶段就报错，不会留到用户运行时。

> `winnt.h` 里 `COMPRESSION_FORMAT_XPRESS = 0x0003`（`0x0002` 是更老的 LZNT1）。
> 打包端和解包端必须一致，否则解压直接失败。

## 单文件版的运行时释放

首次运行释放到 `%TEMP%\WaveTether\rt-<内容哈希>\`（约 25 MB，16 个文件）。

- **退出时不删除** —— 下次启动直接复用，省约 1 秒
- **启动时修剪**其它 `rt-*` 目录（首次解压分支也修剪，不只是复用分支）
- **完整性检查**：清单里 16 项全部存在才算「已释放」，缺一个就整个重来
  （杀软隔离 dll / 手删 / 释放到一半被强杀，都能自愈）
- **探针路径从清单推导**，不写死 `scrcpy-win64-v5.0.1`，升级 scrcpy 后不会每次重新解压
- 顺手清掉废弃的旧布局 `%LOCALAPPDATA%\WaveTether\runtime\`

释放失败会在界面顶部红条里说明真实原因（写入被拒 / 磁盘不足 / 解压失败），
而不是笼统的「找不到 adb.exe」。

## 文件构成

| 文件 | 说明 |
|---|---|
| `main.cpp` | 窗口、消息循环、全部界面绘制、状态机 |
| `adb.cpp` / `adb.h` | ADB 通道、scrcpy 进程管理、自适应缓冲、无线重连 |
| `audio.cpp` | WASAPI 会话枚举、逐会话音量、峰值表 |
| `theme.h` | 配色令牌（由 `tools\gen-theme.py` 从 `Themes\*.xaml` 生成） |
| `netscan.h` | 局域网 5555 扫描（分批，绕开 `fd_set` 上限） |
| `_embedded_tools.inc` | 单文件版释放逻辑（被 `adb.cpp` include） |
| `adb_bootstrap.inc` | 插 USB 后自动 `tcpip 5555` + 无线接管 |
| `settings_save.inc` | 设置序列化 |
| `help_*.inc` | 帮助面板各页正文 |
| `jsonmini.h` | 手写 JSON 小工具（转义安全的跳字符串 / 切对象 / 解析数组） |
| `perf.h` | 诊断日志（`%APPDATA%\WaveTether\perf.log`，2MB 轮转） |
| `gen_embed.py` | 生成内嵌资源（压缩 + 清单 + 内容哈希） |
| `env.bat` / `build*.bat` | 构建 |

`test_*.cpp` / `peak.cpp` 是开发期的一次性验证程序，不参与构建。
