# native/ — WaveTether 原生实现

Win32 + GDI+ 手绘界面，无外部依赖，编译产物是免安装绿色版。

## 构建

```bat
build.bat          :: 便携版：WaveTether.exe（约 0.7 MB）+ tools\ 目录
build.bat bt       :: 蓝牙版：WaveTether-BT-only.exe（同一份代码，只有 VERSIONINFO 文件名不同）
build_single.bat   :: 单文件版：WaveTether-single.exe（约 14 MB，内嵌全部工具）
```

三个脚本都先 `call env.bat` 准备编译环境。

`CMakeLists.txt` 提供同样三个目标的 CMake 构建。CI 用 `tools\cmake_check.bat`
跑一遍并做自检，防止两条构建链悄悄分叉；发布产物仍由 `build.bat` 生成。
两者产物不是逐字节相同（VS 生成器链接默认库的顺序不同，导入表布局随之变化），
但代码与行为一致。

`env.bat` 用 `vswhere` 定位 Visual Studio（找不到再退回常见安装路径），
SDK 头文件路径取自 vcvars 设置的 `%WindowsSdkDir%` / `%WindowsSDKVersion%`，
**不硬编码任何版本号**。

> 构建约定：cmd.exe 解析 `.bat` 使用 OEM 代码页，因此脚本保持 ASCII；
> 展开后含 `(x86)` 的路径不要放进 `for (...)` 或 `( ... )` 块，避免括号提前闭合。

构建中间产物统一进 `native\obj\`，不散落在源码目录。

## 单文件版的内嵌资源

`gen_embed.py` 从 `tools\scrcpy\scrcpy-win64-*\` 里挑出 11 个运行必需的文件
（外加 5 个许可/第三方声明文件，共 16 个），
用 Windows 自带的 XPRESS 压缩（`ntdll!RtlCompressBuffer`，无需第三方库），
写成 RCDATA 资源：

| 资源 | 内容 |
|---|---|
| `1`..`16` | 每个文件一份（压缩后；压不小的按原样存） |
| `IDR_EMBED_MANIFEST` | `序号<TAB>相对路径<TAB>原始大小<TAB>压缩后大小<TAB>SHA-256` |
| `IDR_EMBED_HASH` | 内嵌内容的 SHA-1 前 8 位，决定运行时目录名 |

实测 25.0 MB → 12.8 MB（51%）。`gen_embed.py` 会在打包时做一次压缩往返自检，
压坏了在构建阶段就报错，不会留到用户运行时。

> `winnt.h` 里 `COMPRESSION_FORMAT_XPRESS = 0x0003`（`0x0002` 是更老的 LZNT1）。
> 打包端和解包端必须一致，否则解压直接失败。

## 单文件版的运行时释放

首次运行释放到 `%TEMP%\WaveTether\rt-<内容哈希>\`（约 25 MB，16 个文件）。

- **退出时不删除** —— 下次启动直接复用（实测到窗口出现约 60 ms，重新解压约 140 ms）
- **启动时修剪**其它 `rt-*` 目录（首次解压分支也修剪，不只是复用分支）
- **完整性检查**：16 项都存在、大小一致、**SHA-256 也对**才算「已释放」；
  任何一项不符就重建（杀软隔离 dll / 手删 / 同尺寸替换，都能自愈）
- **重建不先删旧目录**：先完整写到 `rt-<哈希>-new`，成功后再换名。
  旧目录被占用（scrcpy 还在跑、杀软在扫）时本轮直接用新目录，不会把旧目录删成半截
- **探针路径从清单推导**，不写死 `scrcpy-win64-v5.0.1`，升级 scrcpy 后不会每次重新解压
- 顺手清掉废弃的旧布局 `%LOCALAPPDATA%\WaveTether\runtime\`

释放失败会在界面顶部红条里说明真实原因（写入被拒 / 磁盘不足 / 解压失败），
而不是笼统的「找不到 adb.exe」。

## 文件构成

| 文件 | 说明 |
|---|---|
| `main.cpp` | 窗口、消息循环、绘制框架、状态机 |
| `ui_tick.inc` | 定时器逻辑（15 个 TickXxx + `UiTick`） |
| `ui_input.inc` | 鼠标 / 键盘处理 |
| `ui_chrome.inc` | 命中测试、DPI、绘制、托盘菜单 |
| `periodic.h` | 按真实时间计时的周期闸门 |
| `autostart.inc` | 开机自启（全程序唯一的注册表读写） |
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

`test_pure.cpp` 是纯函数单元测试，由 `tools\ci.bat` 编译运行；其他 `test_*.cpp` 是开发期的一次性验证程序，不参与构建。
