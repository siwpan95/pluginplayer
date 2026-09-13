# PluginPlayer — 插件版播放器

> 仓库：<https://gitee.com/siwpan/pluginplayer> ·
> 许可：[GPL-3.0-or-later](LICENSE)（原因见「开源与合规」）·
> 发行版：<https://gitee.com/siwpan/pluginplayer/releases>

用 `potplayer-master`（其实是 [itisyang/playerdemo](https://github.com/itisyang/playerdemo)，
一个「开源版 potplayer」的 Qt5 + FFmpeg + SDL2 播放器 demo）的解码/渲染思路做参考，
用 **FFmpeg 9.0.1 + SDL3 3.4.16** 重写内核，并把**一切功能做成插件**：

```
                    ┌──────────────────────────────────────────────┐
   进程内（C ABI）  │  pluginplayer.exe                            │
   plugins/*.dll ──▶│  ┌────────────┐  ┌───────────────────────┐   │
                    │  │ 插件宿主    │  │ 播放内核               │   │
   osd_stats        │  │ · 生命周期  │  │ · demux / decode       │   │
   video_effects    │  │ · 事件总线  │◀▶│   (libav*)             │   │
   audio_viz        │  │ · 服务注册  │  │ · 音频时钟 / A-V 同步   │   │
   snapshot         │  │ · 命令注册  │  │ · SDL3 渲染 + 音频输出  │   │
   hotkey_ext       │  │ · 帧处理链  │  └───────────────────────┘   │
                    │  │ · OSD 绘制  │                              │
   进程外（JSON-RPC）│  └────────────┘                              │
   host-js/ ───────▶│   控制通道 127.0.0.1:7911                     │
   （Cordis）        └──────────────────────────────────────────────┘
```

**插件有两种形态，能力对等：**

| | 进程内原生插件 | 进程外 JS 插件 |
|---|---|---|
| 产物 | `plugins/*.dll` | `host-js/plugins/*.mjs` |
| 接口 | C ABI（`src/sdk/player_plugin.h`） | Cordis 服务 `ctx.player`（JSON-RPC） |
| 优势 | 零拷贝读改像素、能挂帧处理链、性能最好 | 热重载、生态好、写 UI 方便 |
| 适用 | 图像处理、音频分析、叠加层 | 业务逻辑、远程控制、媒体库 |

两者共享同一套概念（**服务 / 事件 / 可撤销注册**），见 `docs/PLUGIN_GUIDE.md`。

---

## 快速开始

```bat
:: 1) 准备依赖（首次；本机已有压缩包时会直接复用）
F:\Python314\python.exe tools\fetch_deps.py

:: 2) 构建（内核 + 11 个原生插件 + 打包 dist/）
F:\Python314\python.exe build.py

:: 2b) 冒烟测试（编译并运行 tests/ 下两个冒烟程序，几秒钟）
F:\Python314\python.exe build.py --smoke

:: 2c) 出发布物（干净重建 + 清理运行期产物 + 生成插件完整性清单）
::     实测产物 57 个文件 / 146 MB（不带 --release 时 dist/ 会累积日志与自检证据）
F:\Python314\python.exe build.py --release

:: 3) 播放（把视频拖到 run.bat 上，或：）
dist\pluginplayer.exe 你的视频.mp4

:: 4) 一键自检（构建 → 冒烟 → 生成测试视频 → 跑通全链路 → 输出 JSON 报告）
selftest.bat

:: 5) 启动 JS 插件宿主（可选，需要先给播放器开控制通道）
dist\pluginplayer.exe --rpc-port 7911 你的视频.mp4
::   启动时会打印：控制通道: 127.0.0.1:7911   令牌文件: dist\config\rpc-token.txt
cd host-js && npm install && node src\index.mjs --port 7911
::   宿主会自动读取令牌文件并认证（也可用 --token 或 PP_RPC_TOKEN 指定）
```

播放中快捷键：`SPACE` 播放/暂停、`←/→` ±5s（Shift ±30s）、`↑/↓` 音量、
`M` 静音、`0-9` 跳百分比、`[ / ]` 变速、`F` 全屏、`S` 截图、
`O` 显示/隐藏 OSD 叠加层、`H` 帮助、`TAB` 列命令、`P` 插件报告、
`Q`/`ESC` 退出（全屏时先退出全屏）。

### 常用选项

| 选项 | 作用 |
|---|---|
| `--rpc-port N` | 控制通道端口（默认 7911；`0` = 不启动） |
| `--rpc-token HEX` | 指定控制通道令牌（默认每次启动随机生成） |
| `--hidden` | 不显示窗口（配合控制通道做无人值守控制） |
| `--write-dir DIR` | 追加插件可写目录（默认只允许 `dist/`，可重复） |
| `--write-plugin-manifest` | 生成 `plugins/manifest.sha256` 完整性清单后退出 |
| `--allow-network-input` | 允许打开 `http/rtsp/UNC` 等网络输入（**默认拒绝**） |
| `--size WxH` | 初始窗口尺寸（默认 1280x720，范围 160..8192） |
| `--selftest --frames N --out DIR` | 无人值守自检 |

`build.py` 的常用开关：`--release`（干净重建 + 清理运行期产物 + 生成完整性清单）、
`--smoke`（冒烟测试）、`--apitest`（编译插件 API 回归插件）、`--clean`、`--debug`。
`tools/fetch_deps.py --check-update` 只发 HEAD 请求，就能告诉你上游依赖有没有更新。

所有数值参数都会做严格校验，非法值直接报错退出（`exit 2`），不会带着越界值继续跑。

---

## 命令行接口（CLI）

上面那些命令都要敲 `F:\Python314\python.exe build.py ...` 这种长串。`tools/pp_cli.py`
把整个项目包成一个 `pluginplayer` 命令，一次装好后在**任意目录**都能用：

```bat
:: 装一次（默认装到已在 PATH 上的 <Python>\Scripts，不改任何环境变量）
F:\Python314\python.exe tools\pp_cli.py install-cli

:: 之后
pluginplayer doctor                         :: 环境体检（真校验：哈希/版本/端口）
pluginplayer build --release                :: 转发给 build.py
pluginplayer selftest                       :: 零参数自检（样例自动生成）
pluginplayer open 影片.mp4 --hidden         :: 起播放器并开控制通道
pluginplayer status                         :: 另一个窗口里查状态
pluginplayer seek +30
pluginplayer cmd effect.set gray
pluginplayer events --seconds 30
pluginplayer quit
```

两类能力（`pluginplayer --help` 有全量列表）：

| 分组 | 命令 |
|---|---|
| 本地编排 | `open` `selftest` `plugins` `manifest` `build` `host-js` `doctor` `install-cli` `uninstall-cli` `version` |
| 控制通道 | `status` `play` `pause` `toggle` `stop` `seek` `volume` `mute` `speed` `cmd` `commands` `services` `events` `rpc` `quit` |

设计要点（细节与实测证据见 **[docs/CLI.md](docs/CLI.md)**）：

- `pluginplayer 影片.mp4` 直接播（`open` 是默认命令）；`--` 之后原样透传给 `pluginplayer.exe`。
- 每个命令都支持 `--json`；**分级退出码** `0/1/2/3/4/5/6/130`，转发类命令原样返回子进程退出码。
- 参数边界与 `src/core/main.cpp` 一一对齐，非法值在**起进程之前**就退 2。
- 命令垫片是**纯 ASCII + CRLF**（cmd.exe 按 GBK 解读 `.bat`），且解释器路径带引号、
  解析顺序为 `PP_PYTHON` → 安装时记录的路径 → `py -3` → `python`。
- 回归测试：`F:\Python314\python.exe tests\cli_check.py` → 实测 **28/28 通过**。

---

## 开发与验证

一条命令跑完所有检查：

```bat
ci.bat                                       :: Windows 一键（门禁 → Python/C++ 单测 → 冒烟 → 自检 → CLI 回归）
F:\Python314\python.exe tools\ci.py           :: 同上，直接调编排脚本；可 --list / --only / --profile ci
```

`tools/ci.py` 把检查拆成**阶段**，每段自己判断前提是否满足：满足就跑，不满足就记 `[SKIP]`
并说明原因（跳过 ≠ 通过）。所以同一份编排在本机能跑全量、在 Linux 容器里能跑可移植子集，
而且"哪一段在哪个环境跑不了"是显式可见的：

| 阶段 | 环境 | 前提 |
|---|---|---|
| `gates` 机械门禁（`tools/verify.py`，18 条） | 跨平台 | 无 |
| `py-unit` Python 单元测试（108 项） | 跨平台 | 标准库 |
| `cpp-unit` C++ 单元测试（89 项） | 跨平台 | g++ / MinGW |
| `smoke` 冒烟、`build` 构建、`selftest` 自检、`color` 色彩还原、`cli` CLI 回归、`release` 发布构建 | **仅 Windows** | vendor/ 或 dist/ |

### 为什么 CI 只有一半是跨平台的

Gitee Go 的托管构建环境是 **CentOS 8.3 / Ubuntu 20.04**（官方插件的 `build@python` /
`build@gcc`），**没有 Windows runner**。而播放器是 Windows 独占的：MinGW 工具链 +
SDL3/FFmpeg 的 Windows 二进制 + Winsock2。所以：

- `.workflow/pluginplayer-ci.yml` 里的流水线**只跑可移植子集**（门禁 + 两种单元测试），
  这一点写在那个文件的开头，不是遗漏；
- 内核/插件的构建、冒烟、自检、发布构建**必须在 Windows 上有构建机**才跑得起来 ——
  托管环境跑不了，只能在本地 `ci.bat`，或接一台自建 Windows 构建机。

可移植子集能成立，是因为单元测试只覆盖 `util` / `config` / `eventbus` / `registry`
四个**不碰 SDL/FFmpeg/WinAPI** 的模块（它们的 Windows API 调用都包在 `#ifdef _WIN32` 里）。
这个前提由 `portable-modules` 门禁静态保证 —— 因为本机装不了 Linux，跑一遍证明不了，
只能靠静态检查把它钉住。

### 机械门禁（`tools/verify.py`）

这个仓库里有很多"同一件事写在两处"的地方：数值边界在 C++ 与 CLI 各一份、控制通道方法名在
`control.cpp` 与 `ARCHITECTURE.md` 各一份、命令数在插件代码与三处文档里各写一遍……。
散文约束不了它们，所以每条「两处必须相等」的断言都被写成了可执行检查：

```bat
F:\Python314\python.exe tools\verify.py              :: 18 条门禁，<1 秒，不需要构建产物
F:\Python314\python.exe tools\verify.py --list       :: 看有哪些
F:\Python314\python.exe tools\verify.py --selfcheck  :: 对每条门禁做一次故意破坏，断言它必须报红
```

覆盖：`.bat/.cmd` 的 ASCII + CRLF 契约、插件完整性清单、发布载荷口径、C++ ↔ CLI 的 13 项
数值边界、控制通道方法集、CLI 退出码表、CLI 命令表、依赖版本、第三方声明新鲜度、
**可移植性（Linux CI 的前提）**、**CI 引用路径存在性**、文档内链、文档索引、目录树真实性、
命令数、原生插件集、JS 插件数。

**`--selfcheck` 是最有用的一条**：它把仓库复制到临时目录，对每条门禁施加一次针对性破坏
（改掉一个边界值、从文档表里删一行、把插件 DLL 改 1 字节、往目录树里塞一个不存在的文件…），
断言门禁必须报红。**一条永远为绿的门禁等于装饰品** —— 这条自检已经因此抓出过真问题：
`build.py` 的 `make_launcher()` 曾用 `newline="\r\n"` 写入已经含 `\r\n` 的字符串，
生成的 `run.bat` 每行实际是 **CR CR LF**，而"只数裸 LF"的旧检查完全看不出来（已修）。

开发流程、测试层次、门禁细则、改动的验证门槛见 **[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)**；
仓库约定与禁区见 **[AGENTS.md](AGENTS.md)**；文档索引见 **[docs/README.md](docs/README.md)**。

可选装成本地钩子（只影响你自己的仓库配置，`git config --unset core.hooksPath` 撤销）：

```bat
git config core.hooksPath .githooks     :: 提交前自动跑 verify.py
```

---

## 开源与合规（**对外分发前必读**）

随包的 FFmpeg 来自 gyan.dev 的 `full_build-shared`，而**该构建是 GPL v3**：

| 组件 | 版本 | 许可 | 随包文件 |
|---|---|---|---|
| FFmpeg | 9.0.1 `full_build-shared` | **GPL v3** | avcodec / avformat / avutil / swscale / swresample（约 118 MB） |
| SDL3 | 3.4.16 | zlib | `SDL3.dll` |
| Cordis / `@cordisjs/plugin-loader` | 4.0.0-rc.9 / 1.0.0-rc.6 | MIT | 不随 `dist/`（跑在 Node 侧） |
| MinGW-w64 GCC 运行时 | GCC 16.1.0 | GPL v3 + Runtime Library Exception | libgcc / libstdc++ / libwinpthread |

因此 `dist/` 这个发布物是 GPL v3 的衍生作品。对外分发前必须：附许可全文、标明改动、
**提供对应源码的获取方式**（本构建对应 FFmpeg 提交 `bf1b838f2a`）、不得附加更严格的条款。

逐条证据（含"随包 README.txt 原文写的就是 `License: GPL v3`"、随包 LICENSE 的 SHA-256 与
FFmpeg 源码树的 `COPYING.GPLv3` 完全一致）与复核命令见 **[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)**
—— 那是**生成物**，由 `tools/gen_third_party_notices.py` 从 `vendor/` 头文件与随包档案读出来，
改依赖后重跑即可，别手改。

**本项目的许可**：`GPL-3.0-or-later`（[LICENSE](LICENSE) 是 GPLv3 的**逐字原文**，
未做任何改动 —— 许可文本本身不允许修改）。选它的原因就是上面那条硬约束：
随包分发的 FFmpeg 是 GPL v3，发布物整体受 GPL v3 约束，因此本项目**只能**用 GPL 兼容许可。

```
PluginPlayer — 插件版播放器
Copyright (C) 2026 siwpan

本程序是自由软件：你可以按自由软件基金会发布的 GNU 通用公共许可证条款
重新分发和/或修改它，条款为第 3 版，或（由你选择）任何更新的版本。

本程序的分发是希望它有用，但不提供任何担保，甚至不提供可商用或适用于
特定用途的默示担保。详见 GNU 通用公共许可证。

你应该已随本程序收到一份 GNU 通用公共许可证的副本；如果没有，见
<https://www.gnu.org/licenses/>。
```

> 版权行的持有人写的是 Gitee 账号名 `siwpan`；要改成真名或组织名，
> 只改上面那一段即可（**不要**去改 `LICENSE` 文件本身）。
> 若需要非 GPL 的分发，唯一路径是自建 LGPL 配置的 FFmpeg 再替换 `dist/` 里的 DLL
> （见 [DEPLOY.md](DEPLOY.md#1-部署前必须处理的一件事许可)）。

---

## 安全模型

这个播放器把"一切功能插件化"，代价是**接口能力很强**：控制通道能调用任意原生插件命令
（包括写文件和退出进程）。因此下面这些是**默认就打开的**，不是可选项：

| 机制 | 默认行为 |
|---|---|
| 控制通道监听 | 只绑 `127.0.0.1`，并用 `SO_EXCLUSIVEADDRUSE` 防端口抢占 |
| 控制通道认证 | **强制 256-bit 令牌**，每次启动重新生成，写入 `dist/config/rpc-token.txt`；认证前任何方法都不可调用 |
| 单条报文上限 | 4 MB，超限断开 |
| 并发连接上限 | 8 |
| 事件推送 | 只推给已认证连接 |
| 插件写文件 | 只允许写进 `dist/`（用 `--write-dir` 追加白名单） |
| 插件完整性 | 可选清单 `plugins/manifest.sha256`：**文件存在即启用**，未列出或 SHA-256 不符的插件拒绝加载（`--write-plugin-manifest` 生成） |
| DLL 搜索路径 | 启动时 `SetDefaultDllDirectories`，把当前工作目录移出 DLL 解析顺序 |
| 依赖下载 | `tools/fetch_deps.py` 解压前校验 SHA-256，并拒绝含 `..`/绝对路径的归档条目（Zip Slip） |
| 网络输入 | `http/https/rtsp/file/UNC` 等**默认拒绝**，需 `--allow-network-input` |
| 网页控制页 `03-web-remote` | **默认关闭**，需 `PP_WEB_REMOTE=1` 或 `config.enabled=true` 才启动；启动后要求 Host/Origin 白名单 + 令牌 + JSON 请求体 |

**信任边界**：`plugins/*.dll` 与 `host-js/plugins/*.mjs` 就是**可执行代码**，
往这两个目录里放东西等于给自己装后门。控制通道令牌文件本身不是安全边界 ——
本机同用户进程本来就能读到它；真正的边界是操作系统用户权限。

如果你的场景只在本机、且不需要进程外插件，最省心的加固方式是
`--rpc-port 0`（完全不启动控制通道）。

---

## 依赖与来源

| 依赖 | 版本 | 来源 | 用途 |
|---|---|---|---|
| FFmpeg | 9.0.1 `full_build-shared`（gyan.dev，GCC 16.1.0 构建） | 下载 | demux / 解码 / 重采样 / 缩放 |
| SDL3 | 3.4.16（devel-mingw + win32-x64 运行时） | 本地压缩包 | 窗口 / 渲染 / 音频 / 动态加载 |
| Cordis | 4.0.0-rc.9 + `@cordisjs/plugin-loader` 1.0.0-rc.6 | npm | 进程外 JS 插件框架 |
| 编译器 | MinGW-w64 GCC 16.1.0（`F:\mingw64\mingw64\bin\g++.exe`） | 本机 | 构建 |
| 参考源码 | `potplayer-master`（playerdemo） | 本地压缩包 | 内核架构参考 |

> 选 gyan.dev 的 `full_build-shared` 是因为它同时提供
> **MinGW 导入库 `lib/*.dll.a` + 完整头文件（含 configure 生成的 `avconfig.h`/`ffversion.h`）+ 全套 DLL**，
> 且与本机 MinGW 同为 GCC 16.1.0，ABI 完全匹配 —— 开箱即可链接，不需要自己编译 FFmpeg。

### 依赖的更新与 CVE（**发布前请看这段**）

- `tools/fetch_deps.py --check-update` 只发 HEAD 请求，比较上游 `Content-Length`
  与本机档案大小，告诉你"现在有没有值得重新抓取的更新"。抓取用 `--refresh`。
- 抓取/校验流程：解压**前**比对 SHA-256 与 `DEPS_SHA256`；不命中会告警并打印实测哈希；
  解压前还会用 `7z l -slt` 扫描条目，拒绝含 `..` 或绝对路径的归档条目（Zip Slip）。
- 下载改为"先写 `.part`、成功且长度匹配才改名"，避免半途失败留下"够大但损坏"的缓存。

**CVE 核查结论（2026-09-12，逐条核对受影响区间）**

本版随包的 FFmpeg **9.0.1 是当前最新稳定版**（2026-08-12 发布；9.0 分支于 2026-06-26
从 master 切出）。曾担心它带一批已公开 CVE，**逐条核对后结论是：可核查的那批都不影响 9.0.1**。

| CVE | 组件 | 受影响区间（权威口径） | 9.0.1 |
|---|---|---|---|
| CVE-2025-9951 | `jpeg2000dec`（avcodec） | **FFmpeg < 8.0**（上游 `104d6846` / `01a292c7`） | 不受影响 |
| CVE-2026-58049 (CVSS 8.6) | `rasc` `decode_dlta`（avcodec） | **≤ `bcd2c69`**；Snyk 记 `[4.2.1, 9.0.1)` | **已在 9.0.1 修复** |
| CVE-2026-12706 | `rasc` `decode_move`（avcodec） | — | 直读 n9.0.1 源码：**已有**完整边界校验 |
| CVE-2026-66037 | `iamf_parse`（avformat） | **through 8.1.2** | 直读 n9.0.1 源码：**已有** `count_label` 上界校验 |
| CVE-2026-65704 | TY demuxer + shorten | **through 8.1.2** | 不受影响 |
| CVE-2026-65706 | `vf_swaprect`（**avfilter**） | 3.0 – 8.1.2 | 不受影响（且 avfilter **未随包**） |
| CVE-2026-64830 | VobSub demuxer | 2.1 – 8.1.2 | 不受影响 |
| CVE-2026-64833 | S/PDIF muxer | 0.7.1 – 8.1.2 | 不受影响 |
| CVE-2026-64834 | `rtpdec_asf` | 0.6.3 – 8.1.2 | 不受影响 |
| CVE-2026-64831 | `vulkan_hevc` | `[8.0, 9.0.1)` | 已在 9.0.1 修复 |
| CVE-2026-64835 | `adxdec` | `[4.4, 9.0.1)` | 已在 9.0.1 修复 |
| CVE-2025-1594 | `aacenc_tns`（**编码器**） | up to 7.1 | 不受影响（播放器也不用编码器） |

**两点诚实边界**：① 以上是"对已披露且被广泛引用的这一批 CVE 的逐条核查"，不等于
对全部历史 CVE 的穷尽审计；② 上游 master 仍在前进（2026-09-09 的快照已是
`avcodec 63.12.100`，而 9.0.1 是 `63.1.101`），将来会有新 CVE 与新修复 ——
所以 `--check-update` 与 `DEPS_SHA256` 钉值仍是常规卫生手段，值得定期跑。

**为什么此前会误判**：发行版公告（如 openSUSE）把一批 CVE 列在 `ffmpeg-9` 包的更新说明里，
容易被读成"这些影响 9.0.1"。但逐条看受影响区间，多数写的是 **"through 8.1.2"**
（即 8.x 最后一个小版本），说明修复早已合入 9.x 分支。

**可选的升级路径**：随包 9.0.1 与本机 master 快照的**大版本号一致**
（`avcodec 63` / `avformat 63` / `avutil 61` / `swscale 10` / `swresample 7`），
所以若想要更新的代码，从 master 源码构建出的 DLL 是**同名可直接替换**的，
不必重编播放器（大版本相同即 ABI 兼容）。这是备选方案，不是当前必需。

---

## 目录结构

```
pluginplayer/
├─ src/
│  ├─ sdk/player_plugin.h        ★ 插件 ABI（纯 C，进程内插件的唯一契约）
│  ├─ core/                           播放内核 + 插件宿主
│  │  ├─ player.cpp/h                 FFmpeg 解码、A/V 时钟同步、seek
│  │  ├─ app.cpp/h                    SDL3 窗口/渲染/音频输出、主循环、自检
│  │  ├─ host.cpp/h                   PP_HostApi 实现（插件能调的一切）
│  │  ├─ eventbus.cpp/h               主题化事件总线（emit/waterfall/serial/bail）
│  │  ├─ registry.cpp/h               服务表 / 命令表 / 帧处理链
│  │  ├─ pluginhost.cpp/h             插件发现、加载、卸载、热重载
│  │  ├─ osd.cpp/h                    叠加层绘制（文字/面板/矩形/频谱柱）
│  │  ├─ control.cpp/h                ★ 控制通道（JSON-RPC over TCP）
│  │  └─ config.cpp/h  util.cpp/h      配置持久化 / 日志 / PNG 写出
│  └─ plugins/                         11 个原生插件（各自独立 .cpp → .dll）
├─ host-js/                            进程外 JS 插件宿主（Cordis）
│  ├─ src/player-service.mjs           ctx.player 服务（控制通道客户端）
│  ├─ src/index.mjs                    宿主入口（装载/卸载插件）
│  └─ plugins/                         3 个 JS 插件
├─ examples/hello_plugin/plugin.cpp    最小原生插件示例（PLUGIN_GUIDE 里逐行讲解）
├─ tests/smoke_link.cpp                工具链冒烟（SDL3 + FFmpeg 能否链接并解码）
│     smoke_audio.cpp                  音频子系统冒烟（判定环境问题还是代码问题）
│     plugin_apitest/plugin.cpp        插件 API 回归测试插件（插件用，不随发布物分发）
│     cli_check.py                     命令行接口回归（垫片契约 + 退出码 + 真跑控制通道）
│     color_check.py                   色彩还原回归（现造参考素材 + 读像素比对期望 RGB）
│     unit/                            ★ C++ 单元测试（89 项，零依赖的极简 harness）
│     python/                          ★ Python 单元测试（108 项，标准库 unittest）
├─ tools/fetch_deps.py                 依赖准备；rpc_client.py 控制通道测试客户端（带令牌）
│     pp_cli.py                        ★ 命令行接口本体（生成 pluginplayer 命令）
│     verify.py                        ★ 机械门禁（18 条，<1 秒，--selfcheck 证明它们拦得住）
│     ci.py                            ★ CI 编排（分阶段，前提缺失记 SKIP 并说明原因）
│     gitee_release.py                   Gitee 建仓 / 转公开 / 发发行版（幂等，可分步重跑）
│     gen_third_party_notices.py        第三方组件声明生成器（依赖/许可证据有变就得重跑）
│     dsh_bind_workspace.py             把本目录注册成 dsh 工作区（独立小工具，可选）
│     dsh_open_ui.py                    dsh 就绪后用默认浏览器打开 Web UI（独立小工具，可选）
├─ pluginplayer.cmd / pp.cmd           命令垫片（纯 ASCII + CRLF，install-cli 生成）
├─ run.bat / selftest.bat              启动器（拖个文件上来即播）/ 一键自检（build.py 生成）
├─ start-dsh.bat                       本项目启动器（转发 run.bat；双击开播放器 / 拖文件即播）
├─ ci.bat                              一键 CI（跑 tools/ci.py；与 run/selftest 同套生成方式）
├─ build.py                            构建脚本（不依赖 cmake/make；--smoke / --unit / --apitest）
├─ .gitignore                          排除构建产物、第三方依赖、运行期产物与令牌文件
├─ .gitattributes / .editorconfig      行尾与编码约定（.bat/.cmd 钉死 CRLF 与 ASCII）
├─ .githooks/pre-commit                 可选本地钩子：提交前跑 tools/verify.py
├─ .workflow/pluginplayer-ci.yml       Gitee Go 流水线（只跑可移植子集，原因写在文件头）
├─ LICENSE                             GPLv3 原文（逐字未改）
├─ AGENTS.md                          仓库约定、边界与禁区、改动的验证门槛（CLAUDE.md 指向它）
├─ DEPLOY.md                           发布与部署：载荷口径、依赖升级、许可义务、排查
├─ THIRD_PARTY_NOTICES.md              第三方组件许可与来源（**生成物，勿手改**）
├─ docs/README.md                      文档索引：按"我要做什么"选读哪一份
├─ docs/ARCHITECTURE.md                线程模型 / A-V 同步 / 协议 / 安全模型 / 踩坑记录
├─ docs/PLUGIN_GUIDE.md                两种插件的开发指南（含可编译示例）
├─ docs/CLI.md                         ★ 命令行接口参考（命令/退出码/设计原则/排查）
├─ docs/DEVELOPMENT.md                 开发与验证：工具链、测试层次、门禁清单、CI 与边界对照表
└─ dist/                               产物：exe(~510KB) + DLL + plugins/ + config/
```

`examples/hello_plugin` 已实测：按指南里的命令行编译出 71KB 的 DLL，丢进 `dist/plugins/`
后宿主报告 `插件目录共 6 个 DLL：成功 6，失败 0`，说明「不改宿主即可扩展」是真的。

---

## 内置插件

### 原生插件（`plugins/*.dll`，C ABI）

| 插件 | 演示的关键能力 |
|---|---|
| `osd_stats` | `osd.draw` 订阅 + OSD 图元 + 查询媒体/统计 + **消费其它插件的服务**（`hotkeys`） |
| `video_effects` | **帧处理链（waterfall）**：灰度/反色/棕褐/色调分离/边缘/镜像可叠加；`PP_CHAIN_DROP` 丢帧；注册 `effects` 服务 |
| `audio_viz` | 订阅 `audio.samples` 拿 PCM + **自实现 FFT** + 在 `osd.draw` 画频谱/波形（插件不链接任何第三方库） |
| `snapshot` | `acquire_frame/release_frame` 取帧副本 + 宿主 `save_png` 写图 + `config_get/set` 持久化 + 帧处理链画标记框 |
| `hotkey_ext` | 订阅 `ui.key` 并 `consumed=1` **吞掉按键**（BAIL 语义）+ 注册 `hotkeys` 服务 + **调用其它插件的命令** |
| `subtitles` | 订阅内核 `subtitle.ass(_header)` 事件 + `osd.draw`，用 **libass** 渲染完整样式字幕（字体/描边/阴影/对齐/覆盖标签 `\an \b \c…`）到叠加层；`subtitle.load` 命令把"加载外部字幕"请求发给内核；libass 不可用时回退 OSD 真字体 |
| `track_selector` | 列出/切换音视频与字幕轨（`track.list` / `track.select`），通过控制通道暴露轨选择 |
| `audio_device` | 枚举/切换音频输出设备（`audio.list` / `audio.set`），消费内核音频设备管理能力 |
| `video_quality` | **画质链（按需挂帧处理器）**：亮度/对比度/饱和度/HDR 增益原地改写 RGBA；任一参数偏离默认才挂链、回默认即撤销；经 `video.color` 服务消费内核色彩元数据（HDR10/HLG/BT.709/601 矩阵+范围）；`quality.set`/`brightness`/`contrast`/`saturation`/`hdr` 命令 |
| `radio_bridge` | 桥接 [private-radio-stations](https://gitee.com/siwpan/allin) 转播服务：worker 线程轮询 `/api/status`（winsock 直连，不进主线程）、台名→标题栏 + **OSD 状态面板**（转播绿/空闲灰/不可达橙 + 圆点指示 + 宽度自适应）、`radio.follow` 自动跟随收听（开转播自动 open `/hls`、停转播自动 stop）；`radio.status`/`base`/`poll`/`follow`/`playing` 命令 |
| `video_tonemap` | **CPU 色调映射（Phase 1）**：HDR（PQ/HLG）→ SDR 的 BT.2390 族 EETF + gamut 软钳 + sRGB，帧链原地处理；`mode=auto` 只在源是 HDR 时挂链（SDR 源不破坏 NV12/GPU 快路径）；`tonemap.mode`/`strength`/`peak`/`status` 命令。Phase 2 换 libplacebo GPU 后端（见 §23） |

### JS 插件（`host-js/plugins/*.mjs`，Cordis）

| 插件 | 演示的关键能力 |
|---|---|
| `01-event-log` | 通配/按主题订阅内核事件、`ctx.effect` 卸载时落盘统计 |
| `02-auto-resume` | **服务型插件**：`inject: ['player']` 消费服务 + `ctx.resume` 提供服务；断点续播；调用原生插件命令（JS→C） |
| `03-web-remote` | 零依赖 HTTP 控制页（`http://127.0.0.1:7912/`），可播放控制 + 执行任意原生插件命令 |

---

## 验证结果（本机实测，可复现）

### 1. 冒烟测试 `build.py --smoke`

```
[PASS] 冒烟测试 smoke_link 运行退出码 0
  SDL3 version      : 3.4.16 / SDL video driver: windows
  decoder h264/vp9/aac: h264 / vp9 / aac
  SMOKE TEST OK
[PASS] 冒烟测试 smoke_audio 运行退出码 0
  AUDIO SMOKE OK (calls=123 bytes=236160)
[PASS] 冒烟测试全部通过
```

### 2. 全链路自检 `selftest.bat`

```
[PASS] sdl_init              — SDL3 video driver=windows
[PASS] plugins_loaded        — 加载 11 个插件，失败 0 个
[PASS] services_registered   — 服务 5 个
[PASS] commands_registered   — 命令 35 个
[PASS] open_media            — mp4 1280x720 30.00 fps dur=8.00 s codec=h264/aac
[PASS] decode_and_display    — 显示 60 帧（目标 60，可接受下限 59），解码 82 / 播放器丢帧 0 / 插件丢帧 0
[PASS] seek                  — 目标 4.80 s，seek 后位置 4.89 s，新显示 5 帧 (上一帧 index=59)
[PASS] plugin_commands       — snapshot -> 0  stats -> 0  effect.toggle gray -> 0
[PASS] track_switch          — selectTrack(audio,1)=1，切换点解码 1 / 切换后 93（增长 92）
[PASS] render_and_capture    — 渲染帧已抓屏保存: ...render_output.bmp (1280x720)；画面矩形 1280x720@(0,0) 内清屏色占比 0.00%（上限 80%）
[PASS] font_render           — 中文'播'与'?'逐像素差异 692 个像素（阈值 30）—— 差异为 0 即说明中文被降级成 '?'
[PASS] overlay_owner_revoke   — 渲染后 纹理红=1600 对照红=1600（阈值 800）；owner 撤销后 纹理红=0，图层数 1 -> 0
[PASS] subtitle_render       — 近白像素 全帧 5179 / 底部带 5140（阈值 150）；已存 selftest_out/subtitle_render.bmp
[PASS] plugin_hot_reload     — 重载后 订阅 15->15, 命令 35->35, 服务 5->5, 帧处理器 0->0（无泄漏）
[PASS] reopen_media          — 二次打开后解码并显示 10 帧（目标 10，可接受下限 9），被插件丢弃 0 帧
==== 自检结束: 15/15 项通过，跳过 0 项，用时 12.09 s ====
```

产物：`dist/selftest_out/` 下的 `selftest_report.json`、`frame_00{1,2,3}.png`
（解码出的原始帧）、`snapshot_0001.png`（**插件**经 `acquire_frame` + 宿主 PNG 写出保存）、
`render_output.bmp`（SDL 渲染抓屏）。其中 `render_output.bmp` **确实包含画面**，
并被 `render_and_capture` 一项断言把住 —— 可以当真证据看。

**自检的可信度**（这几条是刻意加严的，报告里都能看到）：
- `decode_and_display` 的目标是 `min(--frames, 媒体实际可用帧数)`，下限再减 1
  以容忍 EOF 边界丢掉的最后一帧 —— 而不是"显示 5 帧就算过"。
- 不适用的检查记为 `"skipped": true` 并**不计入通过数**（例如 0.5 秒的短媒体
  会跳过 seek 校验，报告里写 `10/10 项通过，跳过 1 项`，而不是把跳过当通过）。
- `plugin_hot_reload` 会断言**订阅/命令/服务/帧处理器数量完全不变**，泄漏即失败。
- `render_and_capture` 断言**抓屏图里真的有画面**，而不是"SDL 没报错就算过"：
  抓屏成功 + 帧确实上传成功 + 画面矩形内「恰好等于清屏色」的像素占比低于 80%。
  这条是 2026-09-12 补的 —— 在此之前它只断言 `surf != nullptr`，而实测那张"证据图"
  里 **86.60% 的像素恰好是清屏色、纯黑 0.00%**，也就是说画面压根没被画进去，
  它却一直是绿的（根因与修法见 `src/core/app.cpp` 中 `pumpAndUpload()` 的注释：
  自检的 seek 步骤抢先改写了 `lastUploadedIndex_`，导致抓屏前那一次上传被去重跳过）。
  修好后同一指标降到 **0.39% / 0.00%**。判据为什么是清屏色而不是"是不是黑的"：
  黑场是合法内容（YUV 有限范围黑场转出来是 `(0,0,0)` 附近），而清屏色
  `RGB(16,16,20)` 是一个**特定**值，整片都是它只能说明纹理没画上去。
- `plugin_commands` 同时断言**每条命令的返回码**（约定 0 = 成功），失败时给出可操作提示。
  以前只统计"调用了几条命令"，于是 `snapshot -> -1`（写文件被白名单拒）也照样 PASS。
- `reopen_media` 会在同一进程里**二次打开同一媒体**并解码 10 帧，断言照样出画。
  这条是 2026-09-12 补的，钉的是一个真正的黑屏缺陷：`Player::open()` 漏复位
  `demuxFinished_` / `drainSent_`，于是第二个媒体一打开 demux 线程就卡在"已完成"分支
  一个包都不读，同时 `finished()` 当场为真把主时钟冻在 0 —— **永久黑屏无画面**，
  日志里「已打开媒体」紧跟着一行「播放结束」。它会命中任何"在同一实例里换片"的用法
  （控制通道 `player.open`、插件 `open_media`）。故意把两行复位注释掉时，
  这一项立刻报 `0 帧` 红（已实测）；修好后是 `10 帧`。
- 无视频轨的媒体（纯音频）**快速判失败**，不会硬等到 60 秒超时。

### 3. 控制通道 `tools/rpc_client.py` —— 23/23 通过

```
=== 控制通道: 23/23 通过 ===
（认证边界 ×2：错误令牌被拒 / 未认证调用被拒
  基本能力 ×6：ping / player.info / player.stats / topic.subscribe / service.list / plugin.list
  播放控制 ×6：pause / play / seek / position / volume / speed
  命令 ×5：command.list / invoke stats / effect.set / effect.clear / hotkey.list / 不存在
  资源边界 ×2：超长报文被拒 / 并发连接上限）
```

### 4. 插件 API 回归 `build.py --apitest`

`tests/plugin_apitest/plugin.cpp` 编出的 DLL 放进 `plugins/` 后调用命令 `api.check`：

```
[apitest] onLoad: video.frame=#5 app.tick=#6
[apitest] PASS 句柄唯一: video.frame=#5  app.tick=#6
[apitest] PASS 撤销帧订阅后主题订阅仍生效 (99 -> 100)
[apitest] PASS 未知句柄被安全忽略
[apitest] PASS 重复撤销幂等
[apitest] PASS 主题订阅撤销生效
[apitest] api.check 结果: ALL PASS（失败 0 项，帧回调 416 次）
```

（跑完记得把 `plugin_apitest.dll` 从 `dist/plugins/` 删掉 —— 它不属于发布内容。）

### 5. JS 插件宿主

```
[host-js] 已装载 JS 插件: event-log
[host-js] 已装载 JS 插件: auto-resume
[host-js] 已装载 JS 插件: web-remote
[host-js] JS 插件 3 个（失败 0）  控制通道: 已连接 abi=1  position=7.96s paused=0  fps=30.0
[host-js] 服务计数: 请求 6 / 事件 5
[host-js] 收到 --exit-after 12s，按逆序卸载 4 个 Fiber…
[host-js] 已全部卸载
```

`.state/js-event-log.json` 记录了 `{app.tick:31, media.eof:1, player.completed:1}`，
`.state/positions.json` 记录了续播位置 —— 证明事件与状态双向打通。

网页控制页（需 `PP_WEB_REMOTE=1` 显式开启）实测 13/13 通过：

```
[PASS] GET / 正常 Host                    -> 200，页面含令牌输入框，带 CSP/nosniff/X-Frame-Options
[PASS] GET / Host=evil.example            -> 403（DNS rebinding 被拦）
[PASS] GET /api/state 无令牌              -> 401
[PASS] GET /api/state 带令牌              -> 200 + 全套安全头
[PASS] /api/state 令牌正确但 Host=evil    -> 403
[PASS] /api/state Origin=evil.example     -> 403（CSRF 被拦）
[PASS] OPTIONS /api/action                -> 405（不放行预检）
[PASS] POST text/plain                    -> 415（简单请求型 CSRF 被拦）
[PASS] POST 合法请求                      -> 200 且真的执行了命令
[PASS] 未知 action                        -> 400 且不回显输入
```

### 6. 边界与异常用例（实测）

| 用例 | 实测结果 |
|---|---|
| `--size 99999x99999` | **exit 2**，打印"非法的 --size 值…需要 160..8192"（修复前是 ACCESS_VIOLATION 崩溃） |
| `--size 8192x8192 --selftest` | 11/11 PASS；抓屏因像素超限**跳过落盘**（避免写出 1.6 GB 的 BMP），但像素取证照常给出结论（8192x4608 画面矩形内清屏色占比 0.00%） |
| `--frames 0` / `--rpc-port 99999` / `--volume 5` / `--log-level 9` | 均 exit 2 并打印合法范围 |
| 不存在的文件 / 空文件 / 非媒体文件 | 优雅 exit 2，无崩溃 |
| 纯音频（`.m4a`）自检 | **0.6 秒**内 FAIL 并说明"媒体没有视频轨"（修复前要硬等 60–80 秒） |
| 0.5 秒短媒体自检 | `10/10 通过，跳过 1 项`（seek 明确标为未验证），0.96 秒完成 |
| 畸形 JSON（纯文本 / 2MB 巨型包 / 二进制 / 深嵌套） | 返回 `ok:false`，进程存活 |
| 超长单条报文（4.5 MB） | 服务端拒绝并断开，内存不再增涨（修复前工作集可从 124 MB 涨到 571 MB） |
| 第 9 条并发连接 | 被拒绝（上限 8） |
| 第二个实例占用同一 `--rpc-port` | bind 返回 `10048` 并**明确报错**，且**不覆盖**运行中实例的令牌文件 |
| 未认证的裸连接 | 拿不到任何方法、也收不到任何事件 |
| `player.open http://...`（6 种网络形式） | 全部被拒（`--allow-network-input` 可放行） |
| `snapshot` 写项目外目录 / 目录穿越 | 被拒（`return=-1` 并打 WARN），白名单内正常写出 |
| 控制通道压力：7 并发 × 8 秒 | 3431 次请求全部成功，**428 req/s**，p50 16.6 ms / p95 17.4 ms / max 19.2 ms，无崩溃无异常读数 |

### 7. 严格 JSON 解析（9/9 通过）

控制通道的 JSON 取值已从"子串搜索"换成严格词法扫描（只认顶层键、正确处理转义、
拒绝重复键与超深嵌套）：

```
[PASS] 重复键被拒                    {"ok":false,"error":"非法 JSON 请求: 重复键: method"}
[PASS] 未闭合 JSON 被拒              {"ok":false,"error":"非法 JSON 请求: 对象未闭合"}
[PASS] 尾部垃圾被拒                  {"ok":false,"error":"非法 JSON 请求: 对象之后有多余内容"}
[PASS] 键名出现在值里不会误命中（仍执行 ping）  ← 旧实现在这里会调到 params 里那个假 method
[PASS] args 含非字符串被拒            {"ok":false,"error":"args 必须是字符串数组"}
[PASS] args 为字符串数组可正常执行
[PASS] 深嵌套不崩溃且被拒            （5000 层数组，深度上限 32 直接拒绝）
[PASS] 深嵌套之后服务仍正常
[PASS] 字符串形式的 id 不会崩（id 退回 0）
```

### 8. 插件完整性清单（3/3 通过）

`plugins/manifest.sha256` 存在时强制校验；`build.py --release` 会自动生成。

```
未登记就放进 plugins/  →  [ERROR] [plugin_apitest.dll] 未在 plugins/manifest.sha256 中列出（完整性校验拒绝加载）
                          插件目录共 6 个 DLL：成功 5，失败 1    exit=4
改坏已登记插件 1 字节  →  [ERROR] [hotkey_ext.dll] SHA-256 与清单不符
                          （期望 79295e91adde359e…，实测 da99d1fab0453f5d…）  exit=4
重新生成清单后         →  插件目录共 6 个 DLL：成功 5，失败 0    exit=0
```

### 9. 发布构建 `build.py --release`

```
[OK] 已删除 build/ 与 dist/          ← 先干净重建（否则会继承上一轮的残留）
[OK] 已生成 plugins/manifest.sha256（6 个插件）
[OK] 已生成 run.bat、selftest.bat、start-dsh.bat 与 ci.bat（纯 ASCII，CRLF）
[release] 可发布文件 22 个，合计 130 MB
```

对比：不带 `--release` 的 `dist/` 会累积到 **306 MB**（含 `logs/`、`selftest_out*/`、
`test_assets/`），且可能残留上一轮手工放进去的 DLL。实测 `.bat` 产物为
**0 个非 ASCII 字节、CRLF 行尾**（`cmd.exe` 按 GBK 解析 .bat，混入中文会乱码/截断）。

### 10. 命令行接口 `tests/cli_check.py` —— 28/28 通过

```
==== CLI 回归: 28/28 项通过，跳过 0 项，失败 0 项 ====
```

不只是"跑起来没报错"，几条刻意加严的断言：

- **垫片契约**：`shim_text()` 的产物必须**纯 ASCII + 全 CRLF**（真去 `encode("ascii")`
  并数裸 LF 个数），且解释器调用行必须带引号；绝对路径版与 `%~dp0` 版除 root 赋值行外
  必须逐字节一致（防止两份垫片各自漂移）。
- **退出码逐个钉住**：未知命令 2 / 拼错给建议 2 / 非法数值 2 / `--size` 越界 2 /
  媒体不存在 2 / 未知选项 2 / 端口未监听 3 / 认证或超时与连不上**分开**。
- **参数先校验再启动**：`open --size 99999x99999` 必须在起进程之前就退 2
  （测试里把 `run_inherit` 换掉，断言"根本没走到启动那一步"）。
- **`--` 的语法归属**：同样的越界值放在 `--` 之前要被拦下，放在 `--` 之后必须
  **原样落在 argv 末尾**（DSH 的"启动器只解析自己的 flag"原则）。
- **插件完整性**：对 `dist/plugins/*.dll` **逐文件重算 SHA-256** 与清单比对，
  而不是只看清单文件在不在。
- **装卸回归**：`install-cli` → `uninstall-cli` 后目录必须为空；且往目标目录放一个
  **别人手工写的同名 `pluginplayer.cmd`**，卸载时必须跳过、不能误删。
- **真跑控制通道**：`status`（info/stats/volume 齐备）、`seek`（相对 + 绝对）、
  `commands`（断言恰好 35 个）、`cmd`（真实命令退 0 / 不存在退 5）、`events`
  （断言真收到 `app.tick`）。

需要 `dist/` 或运行中播放器的用例在前提缺失时记 `SKIP` 且**不计入通过数**
（跳过 ≠ 通过，与 `--selftest` 的口径一致）。

经命令垫片从**别的目录**调用也实测过（工作目录 `C:\Windows`）：

```
pluginplayer.cmd --version    rc=0   pluginplayer-cli 1.0.0
pluginplayer.cmd status       rc=0   媒体   : test_assets/sample.mp4
pluginplayer.cmd seek +1      rc=0   [OK] 位置 -> 0:07.90
pluginplayer.cmd commands     rc=0   effect.clear ... <plugin#5>
pluginplayer.cmd doctor       rc=0   [OK] Python 3.14.7 @ F:\Python314\python.exe
pp.cmd status                 rc=0   完整状态表（音量 0.800）
```

`quit` 之后复查端口：`port 7911 已释放，播放器已退出`。

### 11. 机械门禁 `tools/verify.py` —— 18/18 通过

```
==== 机械门禁: 18/18 通过，跳过 0 项，失败 0 项 ====

[PASS] bat-ascii-crlf       6 个文件：0 非 ASCII 字节、0 裸 LF、0 孤立 CR（含 start-dsh.bat）
[PASS] plugin-manifest      11 个 DLL 哈希与清单逐一相符
[PASS] plugin-command-count 注册 35 个命令，README/tests/cli_check.py/docs/CLI.md 三处声明一致
[PASS] native-plugin-set    11 个：audio_device, audio_viz, hotkey_ext, osd_stats, radio_bridge, snapshot, subtitles, track_selector, video_effects, video_quality, video_tonemap；dist/plugins 一致
[PASS] js-plugin-count      3 个（01-event-log.mjs, 02-auto-resume.mjs, 03-web-remote.mjs），README 声明与实际一致
[PASS] release-payload      57 个文件 / 145.8 MB，与 README 的 57 / 146 MB 一致
[PASS] portable-modules     21 个文件的 Windows API 全部限定在条件编译分支内（Linux CI 的前提）
[PASS] no-credentials       103 个文本文件已扫描，未发现硬编码凭据
[PASS] arg-bounds-align     13 项边界与 C++ 源码一致（窗口最小边、窗口最大边、自检帧数上限、音量下界 …）
[PASS] rpc-methods-align    实现 23 个方法，与 ARCHITECTURE.md 及 CLI 引用一致
[PASS] exit-codes-align     8 个退出码（0、1、2、3、4、5、6、130）与 CLI 常量一一对应，且都有含义说明
[PASS] cli-commands-align   25 个命令，README 与 docs/CLI.md 的表与实现一致
[PASS] versions-align       FFmpeg 9.0.1；SDL3 3.4.16；PP_ABI_VERSION 3；Cordis 4.0.0-rc.9 —— 与 README 依赖表 / ARCHITECTURE.md 声明一致
[PASS] third-party-notices  与当前依赖一致，且含 GPL v3 义务说明
[PASS] doc-links            34 条相对链接全部可解析
[PASS] docs-index           4 份文档全部收录
[PASS] readme-tree          目录结构里 57 个条目全部存在（按 basename 核对）
[PASS] ci-entrypoints       13 个引用全部存在（.githooks/pre-commit, .workflow/pluginplayer-ci.yml, ci.bat）
```

**门禁自己也要被验证。** `--selfcheck` 对每条门禁做一次故意破坏，断言它必须报红：
把 `.bat` 塞进非 ASCII 字节 / 折成裸 LF、改错 `cli_check.py` 的命令数断言、从 `build.py`
的插件表里删一个、改错 README 的 JS 插件数、把 CLI 的窗口最小边改成 161、从
`ARCHITECTURE.md` 的方法一览里删掉 `app.quit`、从退出码表里删掉 130 那行、往命令表里塞
一个不存在的命令、改错依赖表里的 FFmpeg 版本、手改生成物、加一条死链、加一份没被索引的
文档、往目录树里塞不存在的文件、把已登记插件改 1 字节、从 `dist/` 里删一个文件、
在可移植模块里塞一句 Wind32 API 调用、把流水线引用的工具改个名 ——

```
[PASS] bat-ascii-crlf         拦住了：把 .bat 的行尾折成裸 LF
[PASS] arg-bounds-align       拦住了：把 CLI 的窗口最小边改成 161（与 app.h 不符）
[PASS] plugin-manifest        拦住了：把已登记插件改 1 个字节（必须靠重算哈希发现）
[PASS] portable-modules       拦住了：在可移植模块的空行处塞一句 Windows API 调用
...                                                            （共 19 项）
==== 门禁自检: 19/19 通过，跳过 0 项，失败 0 项 ====
```

这条自检**已经产出了真实收益**，抓出的问题都不是顺手看到的：

- 第一版里"把 `.bat` 折成裸 LF"这个用例始终拦不住，追下去才发现
  `build.py:make_launcher()` 用 `newline="\r\n"` 写入了本身已含 `\r\n` 的字符串，
  于是每个行尾实际是 **CR CR LF**；而只看"裸 LF 数"的旧检查对 `\r\r\n` 完全无感
  （`\n` 都配了 `\r`）。修法是把 `newline` 改成 `""` 让内容原样落盘，
  并给门禁加上"孤立 CR"检查。
- `portable-modules` 门禁自身的两个解析缺陷也是被自检逼出来的：它一开始没跟踪
  非 `_WIN32` 的预处理条件块（`util.cpp` 里嵌套的 `#ifndef WIN32_LEAN_AND_MEAN`
  会把 `#endif` 提前配对掉），又把注释里的 API 名字当成真实调用。
- `versions-align` 原本只判断"版本串在 README 里出现过"，而 README 在 CVE 表、变更记录里
  也会出现同一个版本号 —— 改错依赖表照样过。已收紧为"必须在依赖表那一行里"。

### 12. 单元测试与 CI —— 89 + 108 项通过，本地 CI 9/9 阶段

```bat
F:\Python314\python.exe build.py --unit                        :: C++ 单元测试
F:\Python314\python.exe -m unittest discover -s tests\python   :: Python 单元测试
ci.bat                                                          :: 一键跑全部
```

```
==== 单元测试: 89/89 项通过，跳过 0 项，失败 0 项 ====       ← tests/unit/（C++，零依赖 harness）
Ran 108 tests in 0.900s
OK                                                          ← tests/python/（标准库 unittest）

==== CI: 9/9 阶段通过，跳过 0 项，失败 0 项，用时 137.2s ====  ← ci.bat（本地全量）
==== CI: 3/3 阶段通过，跳过 0 项，失败 0 项，用时 24.8s ====   ← --profile ci（可移植子集）
```

单元测试覆盖的是**纯逻辑与安全策略**（不是"跑起来没报错"）：

| 套件 | 项 | 覆盖 |
|---|---|---|
| `string` | 10 | trim/lower/endsWith（大小写不敏感是刻意的）/split 保留空段/fmtTime/fmtBytes/pathJoin |
| `path` | 11 | 白名单包含判定：前缀欺骗、`..` 穿越、大小写、UNC、`scheme://` 与盘符 `C://` 的区分 |
| `json` | 12 | 严格 JSON：重复键、未闭合、尾随垃圾、01 前导零、深嵌套上限、转义往返、**键名出现在值里不误命中**、数组元素类型、键注入 |
| `hash`/`png` | 7 | SHA-256 已知向量；PNG 独立重解析（签名/IHDR/**逐块 CRC32**/IDAT 解回像素逐字节比对/Adler32） |
| `capture` | 8 | 抓屏取证（`render_and_capture` 的判据）：子区域统计、**pitch 带行填充**（防止按 `w*4` 走）、矩形裁剪、alpha 不参与比较、纯黑必须算 0 而非法参数必须算 −1 |
| `color` | 6 | 色彩矩阵取值与 libswscale 的 `SWS_CS_*` 对齐、**标注优先于分辨率猜测**、未标注按 >576 行规则、RGB/异常空间回退、电平范围映射、矩阵与范围相互独立 |
| `eventbus` | 13 | EMIT/WATERFALL/SERIAL/BAIL 四种语义、短路、owner 撤销、分发中撤销自身 |
| `registry` | 11 | 服务/命令/帧链的注册与按 owner 撤销、优先级顺序、DROP/BAIL 短路、缓冲区边界 |
| `config` | 9 | 注释与空白、重复键、dirty 语义（写同值不置脏）、原子保存、类型解析、超长行截断 |
| Python | 108 | CLI 参数校验与全局选项抽取、令牌解析顺序、垫片字节契约、门禁解析器、生成器口径、**缺 vendor 时必须 SKIP 而不是 FAIL**、dsh 小工具的绑定/打开逻辑 |

C++ 单元测试**刻意不依赖 vendor/**（只编不碰 SDL/FFmpeg 的四个模块），
所以"还没下依赖"的机器和 Linux 容器都能跑 —— 这是 CI 能有意义的前提。

### 13. 启动器定位与中文处理（2026-09-12）

- `start-dsh.bat` 是**本项目启动器**：双击打开播放器窗口，媒体文件拖上去即播；它转发
  `run.bat`，启动与错误处理逻辑只保留一份（由 `build.py:make_launcher()` 生成）。
- 控制台中文曾整片乱码（"搜索"显示成"鎼滅储"）：播放器输出是 UTF-8（源码与编译器的执行
  字符集都是 UTF-8），而 Windows 控制台默认按 GBK(936) 解读。现在程序入口调用
  `SetConsoleOutputCP(CP_UTF8)`，启动器里再加 `chcp 65001` —— 两层保证。
- **含中文的媒体路径**曾打不开（`avformat_open_input 失败 ... (Invalid argument)`）：
  Windows 的 `main(argv)` 由 CRT 从 ANSI(本机 GBK) 命令行转换而来，中文路径进到程序里
  已经不是 UTF-8 字节，而 FFmpeg 的接口期望 UTF-8。现在程序入口用
  `CommandLineToArgvW(GetCommandLineW())` 重取 Unicode 参数、转 UTF-8 后重建 `argv`。
- **OSD 曾整块压在画面上**（用户截图反馈）：`osd.draw` 只知道窗口尺寸，插件没法避开
  画面。现在宿主把 letterbox 后的画面矩形经 `PP_OsdApi.video_rect`（**ABI v2**）暴露
  出来，`osd_stats` / `audio_viz` / 宿主暂停角标都会**优先画进上下黑边**，黑边不足才
  退回原位。顺带修了两个渲染缺陷：几何计算改用 `SDL_GetWindowSizeInPixels`（原先用
  窗口逻辑尺寸，高 DPI 下右/下侧 OSD 会被整块裁掉）；自检抓屏改到 `Present` **之前**
  读回（原先那张"证据图"一直是全黑）。实测效果（1280x900 窗口 + 16:9 视频）：画面精确
  占 y 90~809，顶部三块 OSD 在 0~86、底部频谱/信息行/进度条在 810~900，**画面零遮挡**。
  黑边放不下时（例如全屏 16:9 播 16:9）仍会退回画面内，此时可按 **`O`** 一键整体隐藏
  OSD —— 隐藏时连 `osd.draw` 都不广播，插件连绘制都省了。
  > 上述"零遮挡"只在**有黑边**的组合下成立。源与窗口同为 16:9 时画面矩形≈满窗，
  > OSD 必然压在画面上（实测抓屏可见信息面板盖住素材左上角）—— 这是待办，不是已解决。
  >
  > 另外补一句诚实的账：把抓屏改到 `Present` 之前读回**是必要的但不是充分的**。
  > 2026-09-12 复核发现那张"证据图"里**根本没有画面**（86.60% 是清屏色），因为自检的
  > seek 步骤抢先把 `lastUploadedIndex_` 改成了当前帧，抓屏前那次上传于是被去重跳过。
  > 现在上传改由 `pumpAndUpload(force)` 显式保证，并且这条断言会直接检查像素。

```bat
chcp 936                            :: 先把控制台钉到 GBK（复现条件）
start-dsh.bat --list-plugins        :: 启动播放器
chcp                                :: → Active code page: 65001（修复前此处仍是 936）
```

```text
> pluginplayer.exe --selftest --frames 30 --out selftest_out "G:\...\願廣西平安.mp4"
修复前：rc=2   avformat_open_input 失败: ... (Invalid argument)
修复后：rc=0   [PASS] open_media — 480x852 29.97 fps dur=78.46 s codec=h264/aac
```

### 14. 二次打开媒体必然黑屏（2026-09-12 复现并修复）

**症状**：在**同一个进程里换片**（控制通道 `player.open`、插件 `open_media`，
也就是"先起一个实例、再往里送文件"这种用法）——第二个媒体打开后**永久黑屏无画面**：
没有画面、没有声音、`position` 卡在 0，而且日志里「已打开媒体」紧跟着一行「播放结束」。

**根因**：`Player::open()` 复位了绝大多数跨媒体状态，**漏了两个**：
`demuxFinished_` 与 `drainSent_`。第一个媒体读到 EOF 时它们被置真，于是第二个媒体
一打开：① `demuxLoop()` 进循环第一件事就是 `if (demuxFinished_) { sleep; continue; }`
—— **一个包都不读**；② `finished()` 同时满足（旧实现只要求"队列此刻为空"，而刚 open
时缓冲区当然是空的）→ `masterClock()` 立刻返回被冻结的 `extPts_`(=0) →
`pumpFrame()` 永远取不出帧 → `tex_` 从不创建。App 层状态（`lastUploadedIndex_` /
`completedFired_`）当时是复位了的，**漏的是 Player 层**，所以这个缺陷一直没被自检碰到。

**修复**：

- `Player::open()` 补上 `demuxFinished_` / `drainSent_` 复位（并与新增的排水标志一起复位）。
- `finished()` 不再只看"队列此刻为空"，改为要求**解码器真的排空**：视频/音频解码线程
  处理完 drain 空包且 `avcodec_receive_frame` 返回 EOF 时置 `vDrained_` / `aDrained_`，
  `flushAfterSeek()` 与 `open()` 负责置回。这样"刚 open、还没喂上第一批数据"
  和"主线程恰好把帧取空"都不会再被误判成播放结束（旧实现还会因此**提前**对外发
  `media.eof` / `player.completed`，自动续播类插件会误以为片子放完了）。
- `--selftest` 新增 `reopen_media` 步骤把它钉住（见「验证结果」第 2 节）。

**实测（本机，`--selftest` + 控制通道时间线两路取证）**：

| 用例 | 修复前 | 修复后 |
|---|---|---|
| `reopen_media` 步骤（二次打开后解码 10 帧） | **FAIL，显示 0 帧** | **PASS，显示 10 帧** |
| `player.open` 换片后的 `player.stats` | `position=0.000 decoded=0 vqueue=0 abuf=0` **冻结 4.8 s** | `decoded=17 vqueue=15 abuf=572160` 正常推进 |
| 换片后的日志 | `已打开媒体` → `播放结束`（立刻） | 正常播放，不再提前报完 |
| 播到结尾（seek 到 duration−2s 播完） | — | 仍正常收到 `media.eof` + `player.completed`，`position=104.874/104.875` |

**顺带说明（未改的部分，如实记录）**：原计划里还有一条"首帧上屏前不推进主时钟 /
把 `openAudio()` 推迟到首帧就绪"的加固。实际用控制通道扫了 9 个素材
（852x480 到 794 MB、含纯视频 mp4 与纯音频 m4a）**都没有复现出首开黑屏**，
所以**没有做这处改动** —— 没有复现证据就不改行为。

### 15. 色彩还原回归 `tests/color_check.py` —— 5/5 通过（2026-09-12）

**为什么单开一层**：色彩错了**不会崩、不会报错、不会掉帧**，只会让画面偏色或发灰 ——
`--selftest`、CLI 回归、单元测试**都看不见**。所以补一层：真解一帧、读像素，
跟脚本里独立算出的期望 RGB 比对。

**修的是什么**：内核从不读 `AVFrame.colorspace` / `color_range`，而 `sws_getContext()`
一次性建上下文、**不接受任何色彩信息**，于是 swscale 只能按自己的默认值猜矩阵。
真实文件的矩阵是写在码流/容器里的，忽略它就等于用错矩阵还原 —— SD 尺寸素材尤其容易踩到
（分辨率启发式猜 BT.601，而素材可能明明标着 BT.709）。

**改动**：
- 新增可移植纯函数 `chooseYuvColor()`（`util.h/.cpp`）：**帧标注优先**；未标注时按 FFmpeg
  `vf_scale` 的 auto 规则（> 576 行用 BT.709，否则 BT.601）；范围未标注取有限范围。
  纯逻辑放在 util 里，是为了能在 Linux CI 上跑单测（`tests/unit/test_color.cpp`，6 项）。
- `player.cpp` 在建好 swscale 上下文后调 `sws_setColorspaceDetails()`，把**矩阵系数 +
  电平范围**喂进去（`sws_getContext()` 不给这个机会），并把这两项纳入上下文重建判据
  （素材中途换标注时必须重建，否则会拿旧矩阵还原新画面）。
- 取值优先级链：**帧 → 容器**（`codecpar->color_space`）。后者是保险层 ——
  h264 解码器会用 SPS VUI 覆盖 `avctx`：`colorspace` 仅在
  `colour_description_present_flag` 存在时覆盖，而 `color_range` 只要
  `video_signal_type_present_flag` 存在就覆盖（`libavcodec/h264_slice.c:1119`）。

**关键一招是"纯元数据 A/B"**：两个素材的码流**逐字节相同**，只用 `-c copy` 给其中一个
加上容器标注。播放器若忽略标注，两者输出必然一致 —— 所以这项检查不只验"数值对不对"，
还验"标注确实被读进去了"：

```
[PASS] 中性 有限范围→RGB                   左 (0,0,0) 右 (255,255,255)   ← 16→0 / 235→255
[PASS] 中性 全范围→RGB                     左 (0,0,0) 右 (255,255,255)
[PASS] 饱和色 未标注→按分辨率猜 BT.601      期望 (202,10,32) 实测 (201,8,30)
[PASS] 饱和色 容器标 BT.709→必须用 BT.709   期望 (219,30,30) 实测 (217,28,28)
[PASS] 纯元数据 A/B：同码流不同标注 → 输出确有差异（最大通道差 20）
==== 色彩还原: 5/5 项通过，跳过 0 项，失败 0 项 ====
```

**这项检查确实会红**（刻意复现过）：把 `sws_setColorspaceDetails()` 那一步去掉
（即修复前的行为），标 BT.709 的素材回落成 BT.601 的结果，A/B 两项直接失败：

```
[FAIL] 饱和色 容器标 BT.709→必须用 BT.709   期望 (219,30,30) 实测 (201,8,30)
[FAIL] 纯元数据 A/B：同码流不同标注却给出几乎一样的结果（最大通道差 0）
==== 色彩还原: 3/5 项通过，跳过 0 项，失败 2 项 ====
```

**一处如实纠正**：规划里我以为"发灰"来自**电平范围没处理**。实测推翻了这个判断 ——
swscale 对 `yuv420p`（有限）/ `yuvj420p`（全范围）的默认处理本来就是对的
（实测 16→0、235→255、0→0、255→255 全对）。**真正的缺陷是信号标注的矩阵被忽略。**
所以本轮的修复范围按实测结果收敛到"矩阵 + 容器标注"，没有去改本来就对的范围处理。

**仍未处理（登记在「已知限制」）**：`color_primaries` / `color_trc` 内核完全不读 ——
HDR / 广色域素材的传输函数与色域映射属于 P2-3，不在本轮范围。
> 注：此条是 2026-09-12 当时的状态；`color_primaries`/`color_trc` 的读取与 `SDL_Colorspace` 映射
> 已在 P2-L1（§22）实现。

### 16. 硬件解码（D3D11VA）—— 已接入，实测账如实列出（2026-09-12）

**做了什么**：`openVideoCodec()` 尝试建 D3D11VA 设备 + 挂 `get_format`；
解码帧是硬件格式时用 `av_hwframe_transfer_data()` 下载回内存再走原有链路。
任何一步失败都**自动退回软解**（只打 INFO，不报错）。可用 `PP_NO_HWDEC=1` 关掉。
详见 [docs/ARCHITECTURE.md §2b](docs/ARCHITECTURE.md)。

**为什么必须实测**：本机有 Intel UHD 630 + RTX 3050，`-hwaccel d3d11va` 可用；
但"硬解一定更快"是错觉 —— 下载回内存要拷一整帧，而且 4K 素材的瓶颈往往不在解码，
而在主线程每帧的工作量（4K RGBA 纹理上传 = 33 MB/帧）。

**背靠背 A/B（同一二进制、同一素材，用 `PP_NO_HWDEC` 切换；三个素材各跑一次）**

| 素材 | 硬解 | 墙钟 | 进程 CPU | decoded | displayed | dropped |
|---|---|---|---|---|---|---|
| 1080p60（600 帧） | **ON** | 14.4 s | 8.56 s | 617 | 606 | **6** |
| 1080p60 | off | 11.8 s | **6.00 s** | 652 | 606 | **22** |
| 2160p30 H.264（300 帧） | **ON** | 28.3 s | 17.06 s | 312 | **300** | **11** |
| 2160p30 H.264 | off | 15.0 s | 13.08 s | 324 | **284** | **37** |
| 2160p HEVC 10bit（150 帧） | **ON** | 18.8 s | **12.84 s** | 161 | 150 | 10 |
| 2160p HEVC 10bit | off | 19.5 s | **19.22 s** | 160 | 150 | 9 |

**结论（三条，都是实测口径）**

1. **丢帧明显变少**：1080p60 22→6、4K 300 帧 37→11。这是用户能直接感知的收益。
2. **CPU 只在软解最贵的场景才降**：4K HEVC 10bit **19.2 s → 12.8 s（−33%）**；
   但 H.264 反而升高（1080p60 6.0→8.6 s，4K 13.1→17.1 s）—— 下载+拷贝的成本
   超过了省下的解码时间。**所以"硬解省 CPU"不能一概而论。**
3. **4K 当时跑不过自检的严格口径**（`decode_and_display` 判 FAIL，284/299）。当时把原因
   归到"主线程每帧要传 33 MB 的 RGBA"——**这个归因后来被证明是错的**，见下一节：
   那次 FAIL 里有一半是自检自己量错了（测量路径污染），真因是 NV12 路线没开。

**把上传换成 NV12 + GPU 做 YUV→RGB —— 先回退，修好前提后重新启用，实测是决定性收益** ✅

把解码输出改成 NV12、纹理改成 `SDL_PIXELFORMAT_NV12`、让 GPU 做 YUV→RGB，
理论上同时省掉 CPU 的逐像素转换与 3/4 的上传量（4K 从 33 MB/帧降到 12.4 MB/帧）。
这个过程分了三步，中间那次回退与"假 FAIL"都值得记下来：

1. **第一次尝试：净损失，回退**。`displayed` 300→243、`dropped` 11→78。
   原因不在 SDL，而在**插件形态**：`video_effects` / `snapshot` 当时在启动时就注册
   帧处理器，于是"有插件要改像素"恒成立、NV12 快路径永远走不到，反而每帧多一次
   NV12→RGBA 物化。
2. **先修两个前提**：
   - 让两个插件改为**按需占帧链**（不启用就不注册），链默认为空
     （见 [ARCHITECTURE §6b](docs/ARCHITECTURE.md)）；
   - 修掉**自检自身的测量污染** —— 它为了写 PNG 证据会把每帧物化成 RGBA，
     于是量的是"物化开销"而不是播放管线。这两处不修，NV12 的账怎么量都是错的。
3. **重新启用并实测**：用**真实播放**量（`--selftest` 量不了上传路径 —— 它为写 PNG
   证据会把每帧物化成 RGBA）。隐藏实例 + 控制通道采样 + 读进程 CPU 时间，
   同一素材、同一方法、链都为空，**同一份二进制**只差上传路径
   （用 `PP_NO_NV12=1` 强制回退到 RGBA，避免"拿两次不同构建的数字比"）：

| 素材（各 2 轮，8 s 采样窗口） | RGBA 纹理（基线） | NV12 纹理 + GPU YUV→RGB | CPU/帧 |
|---|---|---|---|
| 2160p30 H.264 | 103.2 / 98.8 ms/帧，8 s 只显示 98/95 帧、丢 85/78 | **29.1 / 25.0 ms/帧，显示 182/212 帧、丢 0** | **−72.9%** |
| 1080p60 | 21.9 / 24.4 ms/帧，显示 407/376、丢 67/94 | **7.8 / 7.2 ms/帧，显示 474/476、丢 0~1** | **−66.5%** |
| 720p30 | 13.7 / 13.4 ms/帧，显示 236/235 | **4.9 / 5.4 ms/帧，显示 238/239** | **−62.7%** |
| 2160p HEVC 10bit（4 s 窗口） | 123.9 ms/帧 | **58.5 ms/帧** | −52.8% |

**结论**：**RGBA 路径的主线程每帧 ~100 ms，跟不上 30 fps 的 33 ms 预算**，于是显示帧数
只有 NV12 的一半、且大量帧被整批跳过（丢 78~94 帧/8 s）；NV12 路径下主线程每帧只有
25~29 ms，**显示帧数直接贴着解码速率走、丢帧为 0**。这就是"高分辨率掉帧卡顿"的根治点。

两点如实说明（避免数字被误读）：

- **绝对值随机器状态漂移**：同一批素材在另一时段测得 NV12 19.5 ms/帧、29.4 fps（当时
  D3D11VA 解码 ~30 fps）；本表那次解码速率掉到 ~23 fps，两边的绝对值一起变慢。
  跨时段只比 **CPU/帧比值**（每轮都是同一二进制、背靠背，比值稳定在 −53%~−73%）。
- **这张表是在"每帧重建纹理"的 bug 修掉之后量的**。复查 diff 时发现早期实现把
  colorspace 的赋值放在"建纹理"分支里、判据里读到的恒为初值 0，于是 NV12 纹理**每帧
  都被销毁重建**（4K 每帧 12 MB 反复分配）—— 这个 bug 只惩罚 NV12 路径，等于是把
  NV12 的账算坏后再去比。已修（`app.cpp` 的 `pumpAndUpload`，判据与赋值同序）。

**顺手纠正的两个假象（都是实测，不是推断）**

- **自检的 FAIL 有一半是它自己造成的**：`decode_and_display` 在 2160p 上曾判 FAIL(284/299)。
  把每帧的物化去掉、再把证据图落盘挪到测量循环之外后，同一份构建变成
  **PASS(300/300)、丢帧 0**。详见下一节的"两个自伤型测量缺陷"。
- **成批丢帧的成因可以精确区分**：`pumpFrame()` 现在会在一次弹掉多帧时打出
  `主时钟 / 本帧 pts / 距上次取帧的间隔 / 队列深度`。实测 720p 上每写一张证据 PNG 就固定
  丢 3 帧、且"距上次取帧"正好 ≈137 ms —— 一眼就能看出是主线程被占住，
  而不是解码供不上（队列深度当时是 13~16/16，说明解码其实是超前的）。

正确性回归没有因此回退：色彩回归仍 **5/5**（含"同码流不同标注"的 A/B，差 19），
自检仍 **11/11**（`render_and_capture` 的清屏色占比 0.00%，证明 NV12 纹理确实画上去了）。

顺手验证到的两条 SDL 事实（已实测）：

- SDL3 3.4.16 的 NV12 纹理可用，且 `SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER`
  **确实生效**：同一份 YUV 按 `BT601_LIMITED` / `BT709_LIMITED` 建纹理，
  读回分别是 `(202,10,32)` / `(219,30,30)` —— 与 swscale 的结果**逐级一致**。
- SDL 3.4.16 **没有** `SDL_SetTextureColorSpace`，色彩空间只能在建纹理时给；
  **NV12 纹理的默认 colorspace 是 `SDL_COLORSPACE_JPEG`（全范围 BT.601）**，
  不显式设置就是错色。

### 17. 两个自伤型测量缺陷 —— 自检曾把自己判 FAIL（2026-09-12）

这两条都属于"**测试工具污染被测对象**"，症状是"代码没坏但门禁红"，很容易被误读成产品缺陷
（本轮就真的被误读过一次）。记在这里，因为下一次很可能再踩。

**(a) 写证据图把帧丢了。** `decode_and_display` 要在第 1/半程/末帧各存一张 PNG 作可视证据。
写一张 720p PNG 实测要 **~137 ms**，这期间主循环完全停摆而主时钟照走，回到循环时队列里
三四帧都已"到点"，`pumpFrame()` 只能整批弹掉、只留最后一帧 —— **每写一张固定丢 3 帧**。
修法：证据帧先攥在内存里（帧对象是引用计数的，留着 `shared_ptr` 即安全），
**等测量循环结束再落盘**。改完 720p `dropped` 6→0、2160p 39→0。

**(b) 无条件物化 RGBA 让自检退化成慢路径。** 早期写法对每一帧都调 `makeFrameView()`
（= NV12→RGBA 的 `sws_scale`），2160p 上每帧几十毫秒 —— 于是 `decode_and_display` 量的是
"物化开销"，而不是播放管线。修法：只有"链上真有处理器"或"这一帧要存证据"时才物化。

**这两条的可复用教训**：自检里任何"每帧都做的重活"都会同时改变被测指标。
判断有没有踩坑，看两个数字 —— **丢帧数是否与某个固定动作的次数成正比**，
以及 `pumpFrame` 诊断里的**"距上次取帧的间隔"是否异常大**（正常应 ≈ 循环周期 2~3 ms）。

### 18. 预热（preroll）—— 修掉"开头固定丢 6 帧"（2026-09-12）

**症状**：任何素材，播放开头都固定丢 6 帧（720p 显示 84/90，2160p 显示 261/300）。
**定位过程（三步，每步都留了对照）**：

1. 先怀疑音频时钟粒度（SDL 音频回调按块取数据，一次跳 43~85 ms > 帧间隔 33 ms）——
   **用无音轨素材对照，丢帧完全一样（6 / 6），假设被否掉**。
2. 再用 3 s 与 10 s 同分辨率素材对照：**丢帧不随时长增长（都是 6 帧）**
   → 说明是**固定启动成本**，不是持续跟不上。
3. 于是按"第一帧到来前主时钟已经跑掉 ~0.2 s"去修：加**预热**。

**做法**：`Player::open()` 后进入预热态，`masterClock()` 在预热期**冻结不动**；
`pumpFrame()` 取到第一帧时解除预热；宿主相应地把**音频设备的启动也推迟到那一刻**
（`App::stepAudioPreroll()`），这样 A/V 一起从 0 起跑，而不是"声音从 0 开始、画面从 0.2 s 开始"。
兜底：预热超过 2 s 自动放开并记 WARN，避免坏码流把时钟永久冻住。

**实测**：720p 显示 90/90、2160p 显示 300/300，**丢帧均为 0**。

**seek 后预热（2026-09-12 补做）**：seek 目标处也要重新解码，之前 seek 后会出现
"时钟跑在画面前面 → 整批丢帧"（4K seek 后一次弹掉 24 帧、落点比目标晚 ~2 s）。
现在 `flushAfterSeek()` 里同样进入预热（主时钟冻在目标点），并把**音频设备暂停**
（`Player::setSeekCallback` → `SDL_PauseAudioStreamDevice`），第一帧上屏时由
`stepAudioPreroll()` 一并恢复 —— 语义与 `dropBefore_`（故意丢目标前 0.1 s 以快出画）
不冲突：预热只冻时钟，解码线程照常丢目标前的帧。
**实测（4K，seek 到 5.0 s）**：seek 响应 position 恰为 5.000（冻结在目标）；
seek 后 5 s `dropped +0`（修复前 24）、显示 +115 帧；`--selftest` 的 `seek` 步骤仍 PASS。

### 20. 真字体 OSD：中文终于不是 `?` 了（2026-09-13，P1-a）

**做法**：`Osd::text` 从 SDL 调试点阵字（`SDL_RenderDebugText`，8×8，只能打 ASCII）
换成 **stb_truetype v1.26** 解析 TTF、烤字形图集后用 `SDL_RenderTexture` 逐字绘制。
随包字体 `assets/fonts/ZCOOLKuaiLe-Regular.ttf`（站酷快乐体，**TrueType `glyf` 轮廓**）。

三条实现要点（都是踩过或规避掉的坑）：

1. **stb_truetype 只支持 TrueType `glyf`，不支持 OTF/CFF。** 选字体必须给 `.ttf`；
   给它 `.otf` 会在 `stbtt_InitFont` 处失败（此时**回退**到调试点阵字，不是崩）。
2. **纯逻辑放 `util.cpp`**：UTF-8 解码、东亚全宽判定、显示宽度 —— 这样它们能进
   Linux CI 的单测（`portable-modules` 门禁只管 util/config/eventbus/registry，
   `font.cpp`/`osd.cpp` 不在其中，可以用 Windows API 与 SDL）。
3. **字体缺失不能变成"没界面"**：加载失败时保留原调试点阵字 + `sanitize()` 路径。
   可用 `PP_FONT=<路径>` 覆盖默认字体。

**实测**

- 自检 **12/12**（新增 `font_render` 步骤），`--selftest` 里中文字形逐像素断言通过。
- **断言能判失败（已注入验证）**：把渲染路径强制退回调试字体后，
  中文"播"与对照的 `???` 逐像素差异**恰好 0**，步骤判 **FAIL**（rc=1，11/12）；
  正常构建下差异 **789 像素** → PASS。**差异为 0 正是"中文被降级成 `?`"的特征。**
- 中途抓到两个真 bug：① 截断的 UTF-8 序列没把码点重置为 U+FFFD（返回了首字节的
  部分值 0x06）；② 抓屏在 `Present` **之后**读，拿到的是上一帧视频画面 ——
  必须 Present 前 `ReadPixels`，否则断言形同虚设（实测两块区域都"整片亮"）。
- 单测 **97/97**（新增 `utf8.*` 8 项：多字节解码、非法序列前进、超长/代理区拒绝、全宽判定）。

**合规如实说明**：随包字体 zip 内**没有**随附 LICENSE，字体 name 表也没有
nameID 13/14 许可字段 —— 目前依据是字体发布方（站酷）的公开免费商用声明与来源目录，
**未取得可逐字引用的许可原文**（已写进 `THIRD_PARTY_NOTICES.md` 的"证据"栏）。
若要最强合规证据，应换成 Noto Sans SC / 思源黑体（SIL OFL 1.1，raw 上带完整 LICENSE，约 8 MB）。

另注：**微软雅黑（`msyh.ttc`）版权属方正，不可随包分发** —— 这是字体方案的第一条红线。

### 21. libass 字幕渲染（P1-e）：完整样式 + 随包字体机制

**架构（不变）**：内核只解码字幕、把字幕以事件总线暴露（`subtitle.ass_header` 每条媒体发一次，含完整 ASS 头与默认样式；`subtitle.ass` 每解出一条 Dialogue 发一次）；渲染在本 `subtitles` 插件内用 libass 完成，经 overlay 图层叠加到视频画面（不碰内核渲染代码）。内核不链接 libass、不升 ABI。

**字体机制（本 libass 0.17.5 / MSYS2 meson 构建实测踩坑后的定稿）**：

1. 最初用 `ass_add_font`（把 TTF 读进内存注册为 memory font）+ `ASS_FONTPROVIDER_NONE`，报 `fontselect: failed to find any fallback with glyph 0x0` —— 字体取不到字形。
2. 对照实验（已全部实测验证）发现三件事：
   - `ASS_FONTPROVIDER_NONE` 会把内存字体一并抑制：memory font 根本不进 fontselect；
   - `ASS_FONTPROVIDER_AUTODETECT` 在本机启用 **DirectWrite（with GDI）** 系统 provider，且会**覆盖**内存字体（回退到系统 Arial）；
   - **`ass_add_font` 注册的 memory font 一旦在渲染时被真正选中，会触发 0xC0000005 访问违规崩溃**（4 种组合全部崩）。
3. 本构建也**没有 `ass_attach_font`**（track attachment API 缺失），内嵌 attachment 路线也走不通。
4. **最终稳定方案**：`ass_set_fonts_dir(lib, "<asset_dir>/fonts")` + `ASS_FONTPROVIDER_AUTODETECT` —— 由系统 provider（DirectWrite / fontconfig）扫描随包字体目录加载，零崩溃、跨平台、无需 fontconfig 配置、不用内存字体。验证时 fontselect 打印 `(HappyZcool-2016, 400, 0) -> HappyZcool-2016` 与 `(700, 0) -> HappyZcool-2016`。

**内核/字体解耦（哨兵替换）**：内核 ASS 头统一用哨兵家族名 `PP_SUBTITLE_FONT_FAMILY = "PluginPlayerSub"`（宏在 `player_plugin.h`）；插件收到 `subtitle.ass_header` 时把该哨兵**替换为随包字体的真实 family 名** `HappyZcool-2016`，使内核与具体字体解耦——内核只发中性哨兵、插件拥有真实字体。换随包字体只改插件侧常量 `PP_BUNDLED_FONT_REAL`。

**字体事实**：随包 `assets/fonts/ZCOOLKuaiLe-Regular.ttf`（站酷快乐体 2016 修订版）的 name 表只有 Mac-Roman nameID 1 = **"HappyZcool-2016"**，与文件名 `ZCOOLKuaiLe` 不同——必须以 name 表为准（这是 P1-a 已记录的"stb_truetype 只认 TrueType glyf"之外的第二处字体坑）。字体缺失不致命：libass 回退系统字体，只是 CJK 可能缺字形。

**发布物**：dist/ 随包 32 个 libass 字幕栈 DLL（libass + freetype / harfbuzz / fontconfig / fribidi / glib / pcre2 / brotli 等依赖闭包），与 8 个插件、FFmpeg/SDL3/MinGW 运行时一并分发。`SOURCES.md`（字体来源/许可说明）已从 `assets/fonts/` 移出，避免 libass 扫描时对其报 `Error opening memory font`。

**实测**

- 验收 harness `tools/test_libass_render.cpp`（复刻插件真实机制：fonts_dir + AUTODETECT，不用 ass_add_font）重编后运行：**rc=0，渲染出 22574 个有效像素**，fontselect 确认随包字体命中（常规 + 粗体），中英混排 + 覆盖标签（`\an8 \b1 \c&H00FF00&`）可见。
- `subtitles` 插件编译 **946 KB、无 `-Wall -Wextra` 警告**；`build.py --release` 全量 8 插件 + 打包通过，发布载荷 **54 个文件 / 146 MB**。
- 门禁不受影响：原生插件数 9、命令数 26 均未变（libass 是新增依赖，已补进 `THIRD_PARTY_NOTICES.md` 并刷新发布载荷口径）。

### 19. 环境变量一览（新增）

| 变量 | 作用 |
|---|---|
| `PP_NO_HWDEC=1` | 关闭硬件解码，强制软解（排障 / A/B 用） |
| `PP_NO_NV12=1` | 强制 RGBA 纹理上传路径（**同一二进制**上做 NV12/RGBA A/B 用） |
| `PP_FONT=<路径>` | 指定 OSD 字体（须为 TrueType `glyf` 轮廓）；未设时用随包字体 |

（既有：`PP_WEB_REMOTE`、`PP_RPC_TOKEN`、`PP_PYTHON`、`PP_CLI_NO_INSTALL`、`PP_ROOT`。）

### 22. P2-L1 画质链 + P2/P3 radio_bridge 集成（2026-09-13）

**P2-L1 内核 HDR 色彩空间映射 + video_quality 画质插件（commit `bab6937`）**

- 内核读 `AVFrame.color_primaries` / `color_trc`（`VideoFrame` 加字段，`player.cpp` 读 codecpar 回退），映射 `SDL_Colorspace`：BT.2020+PQ→HDR10(`0x12002600`)、BT.2020+HLG→BT2020（SDL 无 HLG 枚举，已知限制）、其余按矩阵+范围（与 P2-L1 前行为一致）。
- 新增 `video.color` 服务（ABI 安全的 `get_service`，不进 `PP_ABI_VERSION`、不碰 `PP_MediaInfo`）：内核 App 快照最近帧色彩元数据并暴露 getter。
- 新增 `video_quality` 插件（10 号原生插件）：亮度/对比度/饱和度/HDR 增益，仿 `video_effects` 的**按需挂链**（`qualityActive()` 判非默认才挂，链为空时渲染层走 NV12/GPU 路线，避免每帧多一次 RGBA 物化）；6 命令 + 1 服务。
- HDR 输出重建：`ensureHdrOutput_()` 查 `SDL_GetDisplayProperties`/`HDR_enabled`，仅 HDR 显示器时把渲染器重建为 `SRGB_LINEAR`；否则维持 SDR（SDL 自动 HDR→SDR 色调映射，绝不抛错）。`hdrOutputConfigured_` 守卫只重建一次。
- **诚实边界**：本机 selftest 用 SDL dummy 驱动、无真实 HDR 显示器，HDR 输出重建**无法肉眼验证**，靠单测（`test_color.cpp` 5 项 HDR 映射）+ 渲染器创建对 HDR10 不崩 + 优雅回退 SDR + SDR 无回归 + 门禁全绿覆盖。

**P2 radio_bridge 插件（commit `840b277`）+ P0 app.quit 退出延迟（commit `1a8ac2a`）**

- `radio_bridge`：worker 线程轮询 `/api/status`（winsock 直连，2s 超时，HTTP 不进主线程）；台名→标题栏 `set_window_subtitle` + OSD；`radio.follow` 自动跟随收听（开转播自动 open、停转播自动 stop、恢复自动 `play()`，3s 节流扛瞬时竞态）。
- P0：`app.quit` 后保持连接的客户端退出延迟 30.5s→2.1s（`kIdleTimeoutMs` 30000→1000 + 去掉对客户端 socket 的 `::shutdown` 循环）。

**P3-a 内核网络流探测窗口（commit `2271ab8`）+ radio 面板「本机监听」（radio 仓库 commit `dcf75e1`）**

- 内核：FFmpeg 9 把 `AVFormatContext.analyzeduration` 改名为 `max_analyze_duration`，原代码未设→0，慢启动流（刚开播电台 `/stream`）在拿到完整帧前放弃。修为 3s 探测上限 + 8MB probesize；对本地/热流不增延迟，只救 0.5–3s 才出帧的边界。
- radio 面板：`monitor` 插件 spawn `pluginplayer.exe --hidden --allow-network-input --rpc-port`，RPC 控制 `player.open/stats/stop` + `app.quit`，跟随转播（`relay:start` 自动收听、`relay:stop` 自动停）。
- **决定性实测**：监听默认挂载点定为 **`/hls`**——`/stream` 每个连接都是新的 ffmpeg 冷拉源（首帧 5–15s），探测窗口内必失败；HLS 读 radio 自己的热切片，首次失败后重试即成功。
- 顺带修 radio 应用级闸门既有缺陷：`apps/audio/scripts/check.mjs` 用裸 `js-yaml.load`（默认 schema）不支持 `!!js`，与运行时 `parseCordisYml` 不一致 → 复用 `parseCordisYml`，check 由红转绿。

**P3-b radio_bridge OSD 台名样式（commit `02b62cf`）**

- 状态配色：转播中=绿、空闲=灰、服务不可达=橙；左侧 9px 圆点指示；面板宽度按 `text_width` 实测自适应。
- **关键修复**：`Osd::size()` 的 C 包装成功返回 **1**（SDK 注释"返回 0 = 不可绘制"写反），原实现 `if (!size()) return 0` 把成功当失败、面板完全不画；改为与 `osd_stats` 一致忽略返回值、只校验 `w/h > 0`。
- 验证：`render_output.bmp`（**最终合成帧**，非 `frame_*.png` 原始帧）目视确认左下角橙色不可达面板条。

**P3-c radio_bridge 默认 base 改 8000（commit `f70482c`）**

- 默认 base `127.0.0.1:8760`→`127.0.0.1:8000`，与 private-radio-stations 默认 HTTP 端口一致；端口冲突时仍可用 `radio.base <url>` 或 ini 覆盖。

**验证（P2-L1 + P3 全量回归，2026-09-13）**：verify 18/18、unit 103/103、cli_check 24/24、selftest 15/15；radio 端到端 7/7、`npm run check` PASSED、单测 9/9。

### 23. P2-3 色调映射两阶段：Phase 1 CPU（已交付）+ Phase 2 libplacebo GPU（计划）

**Phase 1（2026-09-14，11 号原生插件 `video_tonemap`，纯数学零依赖）**

- HDR（PQ / HLG）→ SDR 的色调映射跑在**帧链**上：PQ EOTF 逆（ST 2084）/ HLG OETF+OOTF 逆
  → 按源峰值归一 → BT.2390 族 Reinhard-extended EETF（软膝 k=0.18）→ gamut 软钳（沿亮度
  向白点去饱和，防色相偏移）→ sRGB EOTF 逆（piecewise）。
- `mode=auto`（默认）只在源是 HDR（内核 `video.color` 服务 `is_hdr` + PQ/HLG 传输函数）时
  挂帧处理器；SDR 源完全不挂链，**不破坏 NV12/GPU 快路径**。`mode=on` 强制（预览/测试），
  `mode=off` 直通（真实 HDR 显示器想走原生直通时用）。
- 命令：`tonemap.mode`（auto/on/off）、`tonemap.strength`（0..1 与原画面混合）、
  `tonemap.peak`（100..10000 nits，默认 1000）、`tonemap.status`。帧链优先级 90
  （先于 video_quality 的 100，色调映射后再调画质）。
- **诚实边界**：① "BT.2390 族"是 Reinhard-extended 软膝，不是 BT.2390 报告逐段精确实现；
  ② 不做 BT.2020→BT.709 基色矩阵（饱和色会偏淡）；③ 帧链像素是 8-bit RGBA（物化路径），
  PQ 在 8-bit 有量化带隙；④ 显示端是 SDR 还是 HDR 不自动探测（auto 只看源），真实 HDR
  显示器请 `tonemap.mode off` 走 SDL 直通。
- **验证**：`tests/unit/test_tonemap.cpp` 12 项像素级单测（PQ 端点/往返/单调、HLG 已知值、
  EETF 已知值与超界滚降、sRGB 已知值、软钳不变式、端到端已知字节：PQ 100nits→174、
  HLG 128→140、白点→255、strength 混合与单调）→ 单测总数 103→**115**；E2E 实测
  `mode=on` 后 17 帧经帧链处理（`seen=17 processed=17`）；selftest 15/15（11 插件/35 命令
  /热重载无泄漏）；verify 18/18；cli_check 30/30（含 tonemap 接线常驻用例）。

**Phase 2（计划：换 libplacebo GPU 后端，补齐 Phase 1 的四条边界）**

- 可行性已核验（2026-09-14）：MSYS2 mingw64 有 `mingw-w64-x86_64-libplacebo` 7.360.1
  （`libplacebo-360.dll` + 全套头文件含 `d3d11.h`/`shaders/colorspace.h`/`gamut_mapping.h`/
  `tone_mapping.h` + `libplacebo.dll.a` + pkgconfig，包 SHA256
  `4a3e80c3…dc2453`）。本机无 pacman 也没关系——`fetch_deps.py` 直接拉 `.pkg.tar.zst`
  解包进 `vendor/libplacebo/` 即可，不需要 MSYS2 安装。
- 代价：拖运行时依赖 lcms2 / libdovi / shaderc / spirv-cross / vulkan-1（发货约 +6-8 个
  DLL、+15-25 MB），发布载荷口径随之更新。
- 集成点：D3D11 后端（`libplacebo/d3d11.h`）对接 SDL3 渲染器，把 HDR 纹理交给
  `pl_renderer` 做 GPU 色调映射（真 BT.2390 / spline / gamut map / dithering），替换
  Phase 1 的 8-bit CPU 帧链路径。
- **验证前提（为什么现在不做）**：libplacebo 只有 GPU 后端（Vulkan/D3D11/OpenGL），其
  dummy 后端只记录命令、不算像素 → 本机（selftest 用 SDL dummy 驱动、无真实 HDR 显示器）
  **无法对其色调映射输出做像素级断言**，只能证明"能编+不崩"。这与"每条声明配可复现证据"
  的红线冲突，故 Phase 2 等有真实 HDR/Vulkan 测试环境再落地；Phase 1 先以可验证的方式
  解决"SDR 屏上 HDR 素材过曝/发灰"的实际问题。

---

## 与原 playerdemo 的差异

| | playerdemo（参考） | 本项目 |
|---|---|---|
| 框架 | Qt5.12 界面 + qmake | 无 GUI 框架，SDL3 直接画 |
| 解码 API | FFmpeg 3.4（`avcodec_decode_video2` 等，已删除） | FFmpeg 9.0.1（`send_packet/receive_frame`、`AVChannelLayout`） |
| 渲染/音频 | SDL2.0.7 | SDL3.4.16（`SDL_AudioStream` + 频率比变速） |
| 可扩展性 | 功能写死在 `videoctl.cpp` | **全部功能插件化**，11 个原生插件 + 3 个 JS 插件 |
| 外部控制 | 无 | JSON-RPC 控制通道 |
| 精确 seek | 无 | 有（seek 后丢帧对齐，`dropBefore_`） |
| 硬件解码 | 无 | **有**（D3D11VA，自动回退软解；实测减少丢帧，见验证结果 §16） |
| 纹理上传 | 每帧 RGBA（CPU 逐像素转换 + 4K 33 MB/帧） | **NV12 纹理 + GPU 做 YUV→RGB**（4K 12.4 MB/帧；CPU/帧 −53%~−73%，4K 显示帧数 ~2 倍且零丢帧） |
| 音频时钟 | 音频/视频/外部三种主时钟 | 音频优先 + **停滞自动退回墙钟** |
| 验证方式 | 手工点界面 | `--selftest` 无人值守产出 JSON 报告 + 帧 PNG（含**像素级**断言"画面真的画上去了"）；另有 `--smoke` 冒烟、`--apitest` 插件 API 回归、`tests/cli_check.py` CLI 回归、`tests/unit` + `tests/python` 单元测试（197 项）、`tools/verify.py` 18 条机械门禁（含自检）、`ci.bat` 一键编排 |
| 外部控制的安全 | 无外部控制 | JSON-RPC 强制令牌认证 + 报文/并发上限；网络输入默认拒绝 |

---

## 已知限制

### 功能

- 只实现 Windows（Winsock2 控制通道、MinGW 工具链）。内核里除 `util.cpp` 的路径/目录部分外基本可移植。
- **硬件解码是"半零拷贝"**：解码在 GPU，但每帧要用 `av_hwframe_transfer_data()` 下载回
  内存（4K 约 12 MB/帧）。实测它**减少丢帧**但在 H.264 上反而更吃 CPU（见验证结果 §16）。
  **真零拷贝评估过、判定不可行（2026-09-12，实测证据）**：把解码表面直接包成 SDL 纹理
  需要 FFmpeg 与 SDL 共用同一 D3D11 设备 —— ① SDL 建的 device 上 D3D11VA 解码 execute
  直接 `E_INVALIDARG`（SDL 建设备时不带 `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`），而 SDL3
  没有"从外部 device 建渲染器"的入口；② 跨设备包装（FFmpeg 自己 device 的 surface 给
  SDL 采样）违反 D3D11 规则（SRV 必须由纹理所属设备创建），实测渲染时驱动
  **Device Removed**（GPU 设备被移除，进程崩溃）。可行路线只有"渲染层整个换 SDL_gpu /
  原生 D3D11"级别的大改，或等 SDL 支持外部 D3D11 设备，已记录在 ENHANCEMENT_PLAN。
- **4K 播放的实际帧率贴着解码速率走**（NV12 路线，主线程不再卡上限；实测显示帧数是
  RGBA 路径的 ~2 倍且丢帧为 0，见验证结果 §16）。当 D3D11VA 解码速率被 GPU 占用拖慢时
  （本机实测 30→23 fps 的波动），帧率会跟着降 —— 那是解码侧的事，不是主线程。
  **4K HEVC 10bit 仍到不了实时**（≈14 fps）：10bit 硬解下载后的转换仍是瓶颈，
  且位深/色域转换还没接（见下面"色彩只处理矩阵与电平范围"）。
- **seek 之后有预热了**（主时钟冻结 + 音频暂停，见验证结果 §18）。但仍有一个已知的
  小窗口：seek 后**视频流损坏/解码器卡死**时预热会在 2 s 超时后放开时钟（记 WARN），
  这时表现与修复前一致。
- **字幕已接入**：`subtitles` 插件订阅内核 `subtitle.ass(_header)` 事件，用 libass 渲染完整样式字幕（含字体/描边/阴影/对齐/覆盖标签，见验证结果 §21）；内核只解码、把字幕以事件总线暴露，渲染在插件内完成，不进 ABI。
- **色彩：矩阵 + 范围 + 传输函数/色域标注已读，但无色调映射**：P2-L1（§22）起内核已读
  `AVFrame.color_primaries` / `color_trc` 并映射 `SDL_Colorspace`（BT.2020+PQ→HDR10、BT.2020+HLG→BT2020），
  HDR 显示器下渲染器会重建为 `SRGB_LINEAR`。**但**没有 libplacebo 级的显式色调映射（tone-mapping），
  所以 HDR→SDR 的显示侧转换交给 SDL 自动处理；非 HDR 显示器上 HDR 素材仍可能偏曝/发灰。
  完整 libplacebo 色调映射属于后续工作。
- 无播放列表 UI；用控制通道或命令扩展。
- 单文件播放；跨文件续播由 `auto-resume` 插件负责但不做自动切集。
- JS 侧 `npm install` 需要网络（`cordis` 与 `@cordisjs/plugin-loader` 从 npm 拉取）。

### 工程与安全（如实列出，未做或做不彻底的部分）

- **许可被随包依赖"锁"住了**：`LICENSE` 是 `GPL-3.0-or-later`，这不是偏好而是约束 ——
  随包 FFmpeg 来自 gyan.dev 的 `full_build-shared`（GPL v3），发布物整体受 GPL v3 约束，
  所以**不能**改用 MIT/专有许可，除非先换成自建 LGPL 配置的 FFmpeg。
  另外版权持有人当前写的是账号名 `siwpan`，要不要改成真名/组织名由你定。
- **CI 只有一半是自动的**：`.workflow/pluginplayer-ci.yml` 走 Gitee Go，但它的托管构建
  环境只有 Linux（CentOS/Ubuntu），**没有 Windows runner** —— 所以云端只跑门禁与两种单元
  测试。内核/插件的**构建、冒烟、自检、发布构建必须在 Windows 上跑**：要么本地 `ci.bat`，
  要么自建一台 Windows 构建机。**没有任何东西会"自动"在推送后验证 Windows 侧。**
  另外那个流水线文件按官方 schema 编写，但**未在 Gitee Go 上实跑验证过**（开通需要在网页端
  操作并绑定手机号）；Gitee Go 通常还要通过网页"新建流水线"才会接管 `.workflow/` 下的配置。
- **测试仍然很薄（相对内核规模）**：单元测试只覆盖 `util` / `config` / `eventbus` / `registry`
  四个模块 —— 它们够得着"纯逻辑与安全策略"，但**解码、A/V 同步、渲染、控制通道的并发**都
  没有单元测试，只能靠 `--selftest` 的端到端检查兜。**仍然没有覆盖率工具**。
- **`tests/cli_check.py` 覆盖的是 CLI 的行为**（垫片、退出码、真跑控制通道，实跑 28/28）；
  单元测试覆盖的是纯逻辑。两者都不覆盖内核的**解码正确性**（只验证"解出了 90 帧"，
  不验证"解出来的像素对不对"）—— 那需要参考帧比对，目前没做。内核与插件的回归仍靠
  `--selftest` / `--smoke` / `--apitest` 三个入口，是"最小入口"而不是完整测试网。
- **命令行接口不随 `dist/` 发布**：`tools/pp_cli.py` 是仓库侧的运维工具，
  靠 `install-cli` 装进 PATH 目录。`dist/` 保持"22 个文件 / 130 MB"的干净载荷，
  口径与 `build.py --release` 一致（把 CLI 塞进去会让这个口径失真）。
  `pluginplayer.cmd` / `pp.cmd` 是生成物但**纳入版本控制** ——
  这样 clone 下来不带 PATH 也能用，且行尾契约由 `.gitattributes` 钉住。
- **依赖组件的 CVE（已逐条核查，风险等级：常规）**：随包的 FFmpeg 9.0.1 是当前最新稳定版，
  逐条核对受影响区间后确认——此前担心的一批 CVE（`CVE-2025-9951`、`CVE-2026-58049`、
  `CVE-2026-12706`、`CVE-2026-65704`、`CVE-2026-65706`、`CVE-2026-66037`、`CVE-2026-6483x` 等）
  **都不影响 9.0.1**：多数受影响区间止于 **8.1.2**，少数明确标为「fixed in 9.0.1」。
  **SDL3 3.4.16** 亦未发现已知 CVE（Ubuntu MIR：osv-scanner 0 漏洞）。
  逐条依据与两点诚实边界见上文「依赖的更新与 CVE」。
  仍需定期跑 `tools/fetch_deps.py --check-update`：上游一直在前进，新 CVE 与新修复都会出现。
- **avfilter / avdevice 未随包**：`dist/` 只带 `avcodec-63 / avformat-63 / avutil-61 /
  swresample-7 / swscale-10`。这也意味着**任何 avfilter / avdevice 的漏洞对本播放器不可达**
  （例如 `vf_swaprect` 那条），因为那两个库根本不会被加载。
- **RPC 延迟受主循环帧期约束**：请求在主线程串行执行，60fps 下 p50 ≈ 16.6 ms。
  如果将来需要低延迟控制，得改成"主线程只做状态快照、慢操作异步化"。
- **插件完整性清单是可选的**：只有 `plugins/manifest.sha256` **存在时**才校验。
  发布时应当用 `--write-plugin-manifest` 生成并纳入版本控制；**新增/替换插件后必须
  同步更新清单，否则该插件会被拒绝加载**。清单只能发现"文件被改过"，不能证明来源。
  加载仍未使用 `LOAD_LIBRARY_SEARCH_*`（DLL 搜索路径已通过 `SetDefaultDllDirectories`
  把工作目录排除，但插件自身的依赖仍走默认搜索顺序）。
- **ABI 演进规则**：`PP_MediaInfo` / `PP_PlaybackStats` / `PP_EventCtx` / `PP_Plugin`
  没有 `struct_size` 字段，靠 `abi_version` **严格相等**兜底 —— 因此对它们做任何字段
  改动都必须递增 `PP_ABI_VERSION` 并重编全部插件。完整规则与"将来要做部分兼容时的
  正确做法"已写进 `src/sdk/player_plugin.h` 顶部。
- **控制通道会返回完整本地路径**（`player.info` / `/api/state`）：这是**刻意保留**的 ——
  JS 插件的断点续播需要用它做 key，而且只有**已认证**连接才拿得到。
- **JSON 解析仍是自实现**：已从"子串搜索"改为严格词法扫描（只认顶层键、正确处理转义、
  拒绝重复键与超深嵌套），但不构造 DOM，数字仍走 `double`（超大整数会丢精度）。
- **解码错误策略**：`av_read_frame` 连续失败 100 次（约 2 秒）即判定读完并收尾，
  避免损坏文件"永远读不出东西也不结束"。`avcodec_send_packet` 的 EAGAIN 已改为
  "先抽帧再重发同一个包"（旧实现直接丢包）。
- **`openMedia` 没有 `interrupt_callback`**：打开损坏/超大/网络流输入时可能阻塞较久
  （控制通道会因为主线程被占而暂时不可用，10 秒后返回"主线程繁忙"）。
- **网页控制页默认关闭**：想用必须显式开启（`PP_WEB_REMOTE=1`）。这是刻意的，
  因为它能驱动任意插件命令。
- **控制通道的所有客户端都必须带令牌**（`tools/rpc_client.py` 与 `host-js` 已内置；
  自己写的脚本见 `control.h` 的协议说明）。
- **升级旧配置**：从 `plugin.<槽位号>.<键>` 迁到 `plugin.<插件名>.<键>` 是**首次访问时
  惰性迁移**的，旧键会保留在 ini 里不删（不做破坏性清理）。旧键的归属无法可靠推断
  —— 槽位号取决于当时的插件集合 —— 所以迁移只是"尽力而为"，请人工核对一次。
