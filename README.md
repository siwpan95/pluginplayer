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

:: 2) 构建（内核 + 5 个原生插件 + 打包 dist/）
F:\Python314\python.exe build.py

:: 2b) 冒烟测试（编译并运行 tests/ 下两个冒烟程序，几秒钟）
F:\Python314\python.exe build.py --smoke

:: 2c) 出发布物（干净重建 + 清理运行期产物 + 生成插件完整性清单）
::     实测产物 17 个文件 / 127 MB（不带 --release 时 dist/ 会累积日志与自检证据）
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
`H` 帮助、`TAB` 列命令、`P` 插件报告、`Q`/`ESC` 退出（全屏时先退出全屏）。

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
| `cpp-unit` C++ 单元测试（75 项） | 跨平台 | g++ / MinGW |
| `smoke` 冒烟、`build` 构建、`selftest` 自检、`cli` CLI 回归、`release` 发布构建 | **仅 Windows** | vendor/ 或 dist/ |

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
│  └─ plugins/                         5 个原生插件（各自独立 .cpp → .dll）
├─ host-js/                            进程外 JS 插件宿主（Cordis）
│  ├─ src/player-service.mjs           ctx.player 服务（控制通道客户端）
│  ├─ src/index.mjs                    宿主入口（装载/卸载插件）
│  └─ plugins/                         3 个 JS 插件
├─ examples/hello_plugin/plugin.cpp    最小原生插件示例（PLUGIN_GUIDE 里逐行讲解）
├─ tests/smoke_link.cpp                工具链冒烟（SDL3 + FFmpeg 能否链接并解码）
│     smoke_audio.cpp                  音频子系统冒烟（判定环境问题还是代码问题）
│     plugin_apitest/plugin.cpp        插件 API 回归测试插件（插件用，不随发布物分发）
│     cli_check.py                     命令行接口回归（垫片契约 + 退出码 + 真跑控制通道）
│     unit/                            ★ C++ 单元测试（75 项，零依赖的极简 harness）
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
[PASS] plugins_loaded        — 加载 5 个插件，失败 0 个
[PASS] services_registered   — 服务 2 个
[PASS] commands_registered   — 命令 15 个
[PASS] open_media            — mp4 1280x720 30.00 fps dur=8.00 s codec=h264/aac
[PASS] decode_and_display    — 显示 90 帧（目标 90，可接受下限 89；媒体约 240 帧可用），被插件丢弃 0 帧
[PASS] seek                  — 目标 4.80 s，seek 后位置 5.01 s，新显示 5 帧 (上一帧 index=93)
[PASS] plugin_commands       — snapshot -> 0  stats -> 0  effect.toggle gray -> 0
[PASS] render_and_capture    — 渲染帧已抓屏保存: ...render_output.bmp (1280x720)
[PASS] plugin_hot_reload     — 重载后 订阅 4->4, 命令 15->15, 服务 2->2, 帧处理器 2->2（无泄漏）
==== 自检结束: 10/10 项通过，跳过 0 项，用时 3.89 s ====
```

产物：`dist/selftest_out/` 下的 `selftest_report.json`、`frame_00{1,2,3}.png`
（解码出的原始帧）、`snapshot_0001.png`（**插件**经 `acquire_frame` + 宿主 PNG 写出保存）、
`render_output.bmp`（SDL 渲染抓屏）。

**自检的可信度**（这几条是刻意加严的，报告里都能看到）：
- `decode_and_display` 的目标是 `min(--frames, 媒体实际可用帧数)`，下限再减 1
  以容忍 EOF 边界丢掉的最后一帧 —— 而不是"显示 5 帧就算过"。
- 不适用的检查记为 `"skipped": true` 并**不计入通过数**（例如 0.5 秒的短媒体
  会跳过 seek 校验，报告里写 `9/9 项通过，跳过 1 项`，而不是把跳过当通过）。
- `plugin_hot_reload` 会断言**订阅/命令/服务/帧处理器数量完全不变**，泄漏即失败。
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
| `--size 8192x8192 --selftest` | 10/10 PASS；抓屏因像素超限**跳过落盘**（避免写出 1.6 GB 的 BMP） |
| `--frames 0` / `--rpc-port 99999` / `--volume 5` / `--log-level 9` | 均 exit 2 并打印合法范围 |
| 不存在的文件 / 空文件 / 非媒体文件 | 优雅 exit 2，无崩溃 |
| 纯音频（`.m4a`）自检 | **0.6 秒**内 FAIL 并说明"媒体没有视频轨"（修复前要硬等 60–80 秒） |
| 0.5 秒短媒体自检 | `9/9 通过，跳过 1 项`（seek 明确标为未验证），1.4 秒完成 |
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
重新生成清单后         →  插件目录共 5 个 DLL：成功 5，失败 0    exit=0
```

### 9. 发布构建 `build.py --release`

```
[OK] 已删除 build/ 与 dist/          ← 先干净重建（否则会继承上一轮的残留）
[OK] 已生成 plugins/manifest.sha256（5 个插件）
[OK] 已生成 run.bat、selftest.bat、start-dsh.bat 与 ci.bat（纯 ASCII，CRLF）
[release] 可发布文件 17 个，合计 127.0 MB
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
  `commands`（断言恰好 15 个）、`cmd`（真实命令退 0 / 不存在退 5）、`events`
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

### 11. 机械门禁 `tools/verify.py` —— 18/18 通过，自检 19/19

```
==== 机械门禁: 18/18 通过，跳过 0 项，失败 0 项 ====

[PASS] bat-ascii-crlf       6 个文件：0 非 ASCII 字节、0 裸 LF、0 孤立 CR（含 start-dsh.bat）
[PASS] plugin-manifest      5 个 DLL 哈希与清单逐一相符
[PASS] plugin-command-count 注册 15 个命令，README/tests/cli_check.py/docs/CLI.md 三处声明一致
[PASS] native-plugin-set    5 个：audio_viz, hotkey_ext, osd_stats, snapshot, video_effects
[PASS] js-plugin-count      3 个（01-event-log.mjs, 02-auto-resume.mjs, 03-web-remote.mjs）
[PASS] release-payload      17 个文件 / 127.0 MB，与 README 的 17 / 127 MB 一致
[PASS] portable-modules     17 个文件的 Windows API 全部限定在条件编译分支内（Linux CI 的前提）
[PASS] no-credentials       82 个文本文件已扫描，未发现硬编码凭据
[PASS] arg-bounds-align     13 项边界与 C++ 源码一致
[PASS] rpc-methods-align    实现 19 个方法，与 ARCHITECTURE.md 及 CLI 引用一致
[PASS] exit-codes-align     8 个退出码（0、1、2、3、4、5、6、130）与 CLI 常量一一对应
[PASS] cli-commands-align   25 个命令，README 与 docs/CLI.md 的表与实现一致
[PASS] versions-align       FFmpeg 9.0.1；SDL3 3.4.16；PP_ABI_VERSION 1；Cordis 4.0.0-rc.9
[PASS] third-party-notices  与当前依赖一致，且含 GPL v3 义务说明
[PASS] doc-links            27 条相对链接全部可解析
[PASS] docs-index           4 份文档全部收录
[PASS] readme-tree          目录结构里 56 个条目全部存在（按 basename 核对）
[PASS] ci-entrypoints       13 个引用全部存在（.githooks/pre-commit, .workflow/*.yml, ci.bat）
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

### 12. 单元测试与 CI —— 75 + 108 项通过，本地 CI 6/6 阶段

```bat
F:\Python314\python.exe build.py --unit                        :: C++ 单元测试
F:\Python314\python.exe -m unittest discover -s tests\python   :: Python 单元测试
ci.bat                                                          :: 一键跑全部
```

```
==== 单元测试: 75/75 项通过，跳过 0 项，失败 0 项 ====       ← tests/unit/（C++，零依赖 harness）
Ran 108 tests in 0.900s
OK                                                          ← tests/python/（标准库 unittest）

==== CI: 6/6 阶段通过，跳过 0 项，失败 0 项，用时 59.3s ====   ← ci.bat（本地全量）
==== CI: 3/3 阶段通过，跳过 0 项，失败 0 项，用时 24.8s ====   ← --profile ci（可移植子集）
```

单元测试覆盖的是**纯逻辑与安全策略**（不是"跑起来没报错"）：

| 套件 | 项 | 覆盖 |
|---|---|---|
| `string` | 10 | trim/lower/endsWith（大小写不敏感是刻意的）/split 保留空段/fmtTime/fmtBytes/pathJoin |
| `path` | 11 | 白名单包含判定：前缀欺骗、`..` 穿越、大小写、UNC、`scheme://` 与盘符 `C://` 的区分 |
| `json` | 12 | 严格 JSON：重复键、未闭合、尾随垃圾、01 前导零、深嵌套上限、转义往返、**键名出现在值里不误命中**、数组元素类型、键注入 |
| `hash`/`png` | 7 | SHA-256 已知向量；PNG 独立重解析（签名/IHDR/**逐块 CRC32**/IDAT 解回像素逐字节比对/Adler32） |
| `eventbus` | 13 | EMIT/WATERFALL/SERIAL/BAIL 四种语义、短路、owner 撤销、分发中撤销自身 |
| `registry` | 11 | 服务/命令/帧链的注册与按 owner 撤销、优先级顺序、DROP/BAIL 短路、缓冲区边界 |
| `config` | 9 | 注释与空白、重复键、dirty 语义（写同值不置脏）、原子保存、类型解析、超长行截断 |
| Python | 108 | CLI 参数校验与全局选项抽取、令牌解析顺序、垫片字节契约、门禁解析器、生成器口径、**缺 vendor 时必须 SKIP 而不是 FAIL**、dsh 小工具的绑定/打开逻辑 |

C++ 单元测试**刻意不依赖 vendor/**（只编不碰 SDL/FFmpeg 的四个模块），
所以"还没下依赖"的机器和 Linux 容器都能跑 —— 这是 CI 能有意义的前提。

### 13. 启动器定位与中文显示（2026-09-12）

- `start-dsh.bat` 是**本项目启动器**：双击打开播放器窗口，媒体文件拖上去即播；它转发
  `run.bat`，启动与错误处理逻辑只保留一份（由 `build.py:make_launcher()` 生成）。
- 控制台中文曾整片乱码（"搜索"显示成"鎼滅储"）：播放器输出是 UTF-8（源码与编译器的执行
  字符集都是 UTF-8），而 Windows 控制台默认按 GBK(936) 解读。现在程序入口调用
  `SetConsoleOutputCP(CP_UTF8)`，启动器里再加 `chcp 65001` —— 两层保证。

```bat
chcp 936                            :: 先把控制台钉到 GBK（复现条件）
start-dsh.bat --list-plugins        :: 启动播放器
chcp                                :: → Active code page: 65001（修复前此处仍是 936）
```

---

## 与原 playerdemo 的差异

| | playerdemo（参考） | 本项目 |
|---|---|---|
| 框架 | Qt5.12 界面 + qmake | 无 GUI 框架，SDL3 直接画 |
| 解码 API | FFmpeg 3.4（`avcodec_decode_video2` 等，已删除） | FFmpeg 9.0.1（`send_packet/receive_frame`、`AVChannelLayout`） |
| 渲染/音频 | SDL2.0.7 | SDL3.4.16（`SDL_AudioStream` + 频率比变速） |
| 可扩展性 | 功能写死在 `videoctl.cpp` | **全部功能插件化**，5 个原生插件 + 3 个 JS 插件 |
| 外部控制 | 无 | JSON-RPC 控制通道 |
| 精确 seek | 无 | 有（seek 后丢帧对齐，`dropBefore_`） |
| 音频时钟 | 音频/视频/外部三种主时钟 | 音频优先 + **停滞自动退回墙钟** |
| 验证方式 | 手工点界面 | `--selftest` 无人值守产出 JSON 报告 + 帧 PNG；另有 `--smoke` 冒烟、`--apitest` 插件 API 回归、`tests/cli_check.py` CLI 回归、`tests/unit` + `tests/python` 单元测试（183 项）、`tools/verify.py` 18 条机械门禁（含自检）、`ci.bat` 一键编排 |
| 外部控制的安全 | 无外部控制 | JSON-RPC 强制令牌认证 + 报文/并发上限；网络输入默认拒绝 |

---

## 已知限制

### 功能

- 只实现 Windows（Winsock2 控制通道、MinGW 工具链）。内核里除 `util.cpp` 的路径/目录部分外基本可移植。
- 没有做硬件加速解码（`d3d11va`/`dxva2`），4K 高码率可能吃 CPU。
- 不解析字幕（`.srt`/`.ass`），只画 OSD；字幕可作为下一个插件接入（`PP_TOPIC_VIDEO_FRAME` + OSD 已够用）。
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
  靠 `install-cli` 装进 PATH 目录。`dist/` 保持"17 个文件 / 127 MB"的干净载荷，
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
