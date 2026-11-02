# WaveTether

把手机 / 平板的媒体声音接到电脑上，和电脑自己的声音混在一起，用同一个耳机听。
设备声音有**独立音量**，不影响游戏 / 音乐；掉线会自动重连。

免安装绿色版，原生 Win32 + GDI+ 实现，无外部运行时依赖。

---

## 三种交付形态

| 形态 | 交付文件（`dist\` 里） | 体积 | 适用 |
|---|---|---|---|
| **单文件版** | `WaveTether.exe` | 约 14 MB | 一个文件，双击即用。首次运行把内嵌工具释放到 `%TEMP%\WaveTether\rt-<哈希>\`（约 25 MB），之后复用；每次启动逐文件校验 SHA-256，被改动或损坏会自动重新释放 |
| **便携版** | `WaveTether-1.1.0-portable.zip`（解开是 `WaveTether.exe` + `tools\`） | zip 约 11 MB（解开约 0.7 MB + 25 MB） | 完全不写临时文件，删掉文件夹就彻底干净 |
| **蓝牙版** | `WaveTether-BT-only.exe` | 约 0.7 MB | 只发一个 exe，蓝牙可用；ADB 通道提示「缺 tools」但不影响使用 |

源码构建产物是 `native\WaveTether.exe`（便携）、`native\WaveTether-BT-only.exe`（蓝牙版，`build.bat bt`）
和 `native\WaveTether-single.exe`（单文件），
打包时改名为上表的名字。构建见 [`native/README.md`](native/README.md)。

仓库里的 `dist/SHA256SUMS.txt` 对应的是 Release 附件；clone 后 dist 下没有 exe / zip 属正常。

## 两条通道

| | 蓝牙（A2DP） | ADB（scrcpy） |
|---|---|---|
| 延迟 | 约 150~300 ms | 默认约 220 ms，可调到约 80 ms（有线约 40 ms） |
| 音质 | SBC/AAC | Opus 48 kHz |
| 多设备 | 一次一台 | 同时多台，各自独立音量 |
| 需要 | 配对一次 | 开 USB 调试；首次插一次线授权 |

同一台手机两条链路不会同时出声：ADB 升级成功时会主动断开这台手机的蓝牙（避免双声）。
不同手机可以一台走蓝牙、一台走 ADB，互不影响。

## 为什么没有 LE Audio

本版走经典蓝牙 A2DP 和 ADB/scrcpy 两条路。LE Audio（LC3）理论上延迟和功耗更有优势，但它要电脑蓝牙芯片、驱动和手机端同时支持才可用；
我们手头没有支持 LE Audio 的设备，无法实测连接、延迟、麦克风回传和多设备切换。
没有实测就加上，在不支持的单机上容易出现连不上、无声、延迟异常，反而比现在更难排查，所以本版不做。
等有可测试设备后再评估。

## 功能

- **独立音量**：只调设备那条音频会话，不动 Windows 全局音量
- **多设备同时播放**：每台一个 `scrcpy` 实例、一条独立音频会话
- **逐设备音量**：右侧「全局 / 单个」分段开关，左侧点行选中、点 `▶/■` 启停
- **输出设备优先级**：连接时切到优先列表第一台可用的，断开自动还原
- **自适应缓冲**：按链路质量自动加减缓冲；挡位 激进 / 积极 / 标准 / 保守
- **双通道热备**（实验性）：备用通道闲时重建，换缓冲时听感无中断
- **断线自愈**：意图集合 + 5 秒重连 + 指数退避 + 延迟重启（等设备端释放音频输入）
- **无线自动接管**：插一次 USB 自动 `tcpip 5555` + `adb connect`，之后无需再插线；可在设置里关掉自动开端口
- **Android 11+ 无线调试配对**：`adb pair` 配对一次，手机重启也不用再插线
- **蓝牙页 = 总设备列表**：按 MAC 记住所有见过的设备（含只用过 ADB 的），改名自动同步，可删除
- **局域网重发现**：IP 变了会扫网段找回同一台设备（按蓝牙名认）
- **蓝牙抢路由自愈**：检测到媒体流仍走 A2DP 就自动断开蓝牙端点，避免"没声音"
- **首次引导向导**、**顶部提示条**（红/黄/蓝分级，可直达自检）、**帮助面板**（10 个主题，每个都是「结论先行 → 怎么做 → 为什么」）、**自检独立窗口**（可拖到旁边对照操作）
- 双主题、托盘常驻、开机自启、不抢焦点

## 一个必须知道的边界

**Windows 不允许把「设备声音」单独送到某个输出设备。** 设备音频由系统渲染到**当前默认输出设备**。
所以「连接时切换到优先级第一的设备」是切换整个系统的默认输出，断开时再还原。
不勾选这个功能就是完全跟随系统默认，不修改任何系统设置。

## 目录

```
CMakeLists.txt           CMake 构建（与 build.bat 等价，CI 里用来交叉验证）
native/                  当前实现（原生 Win32）
  main.cpp               窗口、消息循环、绘制框架、状态机
  ui_tick.inc            定时器逻辑（15 个 TickXxx + UiTick）
  ui_input.inc           鼠标 / 键盘处理
  ui_chrome.inc          命中测试、DPI、绘制、托盘菜单
  periodic.h             按真实时间计时的周期闸门
  autostart.inc          开机自启（全程序唯一的注册表读写）
  adb.cpp / adb.h        ADB 通道、scrcpy 进程管理、自适应缓冲、无线重连
  audio.cpp              WASAPI 会话枚举、逐会话音量、峰值表
  theme.h                配色令牌（由 tools/gen-theme.py 从 Themes/*.xaml 生成）
  netscan.h              局域网 5555 扫描
  pure.h                 纯函数（解析 / 钳制 / JSON 判定 —— 能单测的那部分）
  test_pure.cpp          纯函数单测（64 项断言）
  version.h              版本号唯一来源（app.rc 与打包脚本都读它）
  gen_embed.py           生成单文件版的内嵌资源（XPRESS 压缩 + 清单 + 内容哈希）
  env.bat / build*.bat   构建
tools/                   开发期脚本
  ci.bat                 一键全检（单测 → 构建 → selftest → 静态分析 → 打包 → 校验）
  cmake_check.bat        CMake 构建 + 三个 exe 的自检（两条构建链交叉验证）
  analyze.bat            静态分析（MSVC /analyze）
  check_analyze.py       按（文件, 告警码）逐项不得超过 tools/analyze_baseline.txt
  check_legal.py         许可一致性（notices / dist / 内嵌清单 / zip 四处对账）
  make_dist.py           打包（--verify 校验 SHA256 并跑一遍 dist 里的 exe）
  gen-theme.py           从 Themes/*.xaml 生成 native/theme.h
Themes/                  设计 token 源（Dark.xaml / Light.xaml），只用于生成主题
```

## 运行数据位置

| 内容 | 路径 |
|---|---|
| 设置 | `%APPDATA%\WaveTether\settings.json` |
| 诊断日志（始终写入，单次运行超过 2 MB 轮转为 `perf.log.old`） | `%APPDATA%\WaveTether\perf.log` |
| 详细 adb 调用日志（仅 `WT_TRACE=1` 时） | `%APPDATA%\WaveTether\adb.log` |
| 单文件版运行时文件 | `%TEMP%\WaveTether\rt-<内容哈希>\` |

退出时不删除运行时目录（下次启动更快）；换版本时旧目录在下次启动自动清理。
在「帮助 → 运行时文件」里能看到实际路径和详细说明。

## 许可证

本项目采用「源码可见、仅允许使用」的许可，**不是开源许可证**：可以免费运行（含公司内部商业使用），
可以自用修改，但不得再分发原版或修改版。全文见 [`LICENSE`](LICENSE)。

第三方组件（scrcpy / FFmpeg / SDL3 / ADB 等）仍按各自许可（Apache-2.0 / LGPL-2.1 / Zlib），
见 `dist/THIRD-PARTY-NOTICES.txt` 与 `dist/LICENSE-*.txt`。
