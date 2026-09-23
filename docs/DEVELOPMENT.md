# 开发与验证

改这个仓库之前先读 [ARCHITECTURE.md](ARCHITECTURE.md) 的前两节（分层、线程模型）。
本文件讲的是**怎么改、改完怎么证明没改坏**。

---

## 1. 工具链与前置

| 需要什么 | 本机位置 | 怎么覆盖 |
|---|---|---|
| 编译器 | `F:\mingw64\mingw64\bin\g++.exe`（MinGW-w64 GCC 16.1.0） | 环境变量 `PP_GXX` |
| Python | `F:\Python314\python.exe`（3.14.7） | 用绝对路径调用即可 |
| Node.js | PATH 上的 `node`（host-js 需要） | 环境变量 `PP_NODE` |
| SDL3 + FFmpeg | `vendor/`（不入库，需生成） | `tools/fetch_deps.py --vendor <目录>` |
| 7-Zip | 用于解压 FFmpeg 归档 | 见 `fetch_deps.py` 的 `SEVENZ` 候选表 |

一次装好 CLI 之后，下面所有命令都能写成 `pluginplayer <子命令>`：

```bat
F:\Python314\python.exe tools\pp_cli.py install-cli
pluginplayer doctor          :: 体检：Python/编译器/Node/vendor/dist/插件哈希/端口/命令
```

`doctor` 的 16 项都是真跑的（起进程取版本、逐文件重算 SHA-256、真 connect 一次端口），
不是看文件名猜。**改动环境后先跑它**，比在构建报错里找线索快。

---

## 2. 从零到能跑

```bat
F:\Python314\python.exe tools\fetch_deps.py     :: 1) 准备 vendor/（首次必须）
F:\Python314\python.exe build.py                :: 2) 编译内核 + 8 个插件 + 打包 dist/
F:\Python314\python.exe build.py --smoke        :: 3) 工具链冒烟（SDL3/FFmpeg 能否链接并解码）
pluginplayer selftest                           :: 4) 全链路自检（样例自动生成，产出 JSON 报告）
```

`fetch_deps.py` 的防护值得知道：解压**前**用 `7z l -slt` 扫条目，拒绝含 `..` 或绝对路径的
归档条目（Zip Slip）；下载走"先写 `.part`、长度匹配才改名"；SHA-256 是 **TOFU** 式的
（钉的是"本机曾成功构建过的那份"，能发现内容被换，不能证明来源可信）。

---

## 3. 测试与验证的层次

本仓库**不引入第三方测试框架**（C++ 用 `tests/unit/harness.h`，Python 用标准库 `unittest`）。
验证分几层，各管一段，别指望任何一层覆盖全部：

| 层 | 入口 | 测什么 | 多久 | 需要什么 |
|---|---|---|---|---|
| 工具链冒烟 | `build.py --smoke` | SDL3/FFmpeg 能否链接、能否解出 h264/vp9/aac、音频子系统能否出声 | 几秒 | 编译器 + vendor/ |
| 插件 API 回归 | `build.py --apitest` | 句柄唯一性、撤销订阅的隔离性、未知句柄被安全忽略、重复撤销幂等 | 几秒 | 编译器 |
| **C++ 单元测试** | `build.py --unit` | 89 项：字符串/路径白名单/严格 JSON/SHA-256/PNG 结构/抓屏取证像素占比/色彩矩阵取值/事件总线四模式/注册表撤销/配置持久化 | 3 秒 | 编译器（**不需要 vendor/**） |
| **Python 单元测试** | `-m unittest discover -s tests/python` | 108 项：CLI 参数校验、令牌解析、垫片字节契约、门禁解析器、生成器口径 | 1 秒 | Python 标准库 |
| **全链路自检** | `pluginplayer selftest` 或 `--selftest` | 插件加载、服务/命令注册、开媒体、解码显示帧数、seek 精度、插件命令（含返回码）、**抓屏且断言图中确有画面**、二次打开媒体、热重载**无泄漏** | 4–5 秒 | 构建产物（媒体会自动生成） |
| **色彩还原回归** | `tests/color_check.py` | YUV→RGB 的矩阵与电平范围（现造参考素材 + 读像素比对期望 RGB + 同码流不同标注的 A/B） | 3–5 秒 | Windows + 随包 ffmpeg + 构建产物 |
| **CLI 回归** | `tests/cli_check.py` | 垫片契约、分级退出码、`--` 透传、doctor、插件哈希、装卸回归、控制通道真跑 | 数秒 | 构建产物（控制通道用例还需运行中的实例） |
| **机械门禁** | `tools/verify.py` | 静态一致性：文档↔实现、边界两处一致、可移植性、生成物新鲜度 | < 1 秒 | 无（只需仓库） |

上面这些都由 `tools/ci.py` 串起来（见 §5）：本地跑全量，Linux 容器跑可移植子集。

自检与 CLI 回归的口径一致：**不适用的检查记为 SKIP 且不计入通过数**。
跳过不是通过 —— 报告里会写「N/N 项通过，跳过 M 项」，M 不为零时你该知道为什么。

几条刻意加严的断言（报告里都看得到）：

- `decode_and_display` 的目标是 `min(--frames, 媒体实际可用帧数)`，下限再减 1 以容忍
  EOF 边界丢帧；不是"显示 5 帧就算过"。
- `plugin_hot_reload` 断言订阅/命令/服务/帧处理器数量**完全不变**，泄漏即失败。
- 无视频轨的媒体**快速判失败**，不硬等 60 秒超时。
- `tests/cli_check.py` 里 `--size` 越界的用例会把 `run_inherit` 换掉，
  断言"根本没走到启动那一步"。
- PNG 写出的用例不满足于"文件写出来了"：它**独立重新解析**产物，逐块重算 CRC32，
  再把 IDAT 解回扫描线**逐字节比对像素**。
- 路径白名单的用例对着攻击面写：前缀欺骗（`…/dist_evil` vs `…/dist`）、`..` 穿越、
  大小写、UNC、以及别把盘符 `C://x` 误判成 URL 协议。

### 单元测试为什么只覆盖这四个模块

`util` / `config` / `eventbus` / `registry` 是**不碰 SDL / FFmpeg / WinAPI** 的模块
（它们的 Windows API 调用包在 `#ifdef _WIN32` 里）。好处有两个：

1. 它们正好承载"纯逻辑与安全策略"——严格 JSON 解析、写入白名单、撤销语义，出错的
   后果都是静默的（参数走私、白名单绕过、插件卸载后残留），值得逐条钉住；
2. 一套源码在 Windows(MinGW) 与 Linux(g++) 上都能编能跑 —— 托管 CI 只有 Linux，
   这是 C++ 测试能进 CI 的唯一办法。

解码、A/V 同步、渲染、控制通道的并发**没有单元测试**，只能靠 `--selftest` 的端到端检查。
这不是省事，是因为它们要么依赖真实媒体与设备，要么依赖多线程时序，单元测试里做不出可信的
断言（假的断言比没有更糟）。

---

## 4. 机械门禁（`tools/verify.py`）

散文约束不了两处一致性。这个脚本把每一条"两处必须相等"的断言变成可执行检查 ——
跑一次就知道文档有没有跟实现走散。设计参考了 `deepseek-harness-master` 的
`scripts/verify-*.ts` 一族（那边有 150+ 个门禁脚本 + `pnpm run doc-sync` 串起来；
本仓库规模小，收成一个文件）。

```bat
F:\Python314\python.exe tools\verify.py                 :: 全跑
F:\Python314\python.exe tools\verify.py --list          :: 看有哪些门禁
F:\Python314\python.exe tools\verify.py --only doc-links,arg-bounds-align
F:\Python314\python.exe tools\verify.py --json          :: 机器可读
F:\Python314\python.exe tools\verify.py --verbose       :: 打印每条检查的依据
```

门禁分三类：

**一致性**（同一事实写在两处）

| 门禁 | 断言 |
|---|---|
| `arg-bounds-align` | `app.h` / `main.cpp` / `control.h` 里的数值边界 == `tools/pp_cli.py` 的具名常量（13 项） |
| `rpc-methods-align` | `control.cpp` 的方法集 == `ARCHITECTURE.md` 的「方法一览」== CLI 引用的方法名 |
| `exit-codes-align` | `docs/CLI.md` 的退出码表 == `pp_cli.py` 的 `E_*` 常量 |
| `cli-commands-align` | README 与 `docs/CLI.md` 的命令表 == `pp_cli.command_table()` |
| `versions-align` | README 记的版本 == `ffversion.h` / `SDL_version.h` / `node_modules` / `PP_ABI_VERSION` |
| `plugin-command-count` | 插件 `register_command` 的数量 == README / `cli_check.py` / `docs/CLI.md` 声明的数字 |
| `native-plugin-set` | `build.py` 的 `PLUGINS` == `src/plugins/` 目录 == `dist/plugins/` 的 DLL |
| `js-plugin-count` | README 声明的 JS 插件数 == `host-js/plugins/*.mjs` 数量 |

**契约**（约定必须被遵守）

| 门禁 | 断言 |
|---|---|
| `bat-ascii-crlf` | 每个 `.bat`/`.cmd` 纯 ASCII + 全 CRLF + **无孤立 CR**（`cmd.exe` 按代码页解读；`\r\r\n` 只数裸 LF 是看不出来的） |
| `plugin-manifest` | `dist/plugins/manifest.sha256` 与在盘 DLL 逐一相符，且无未登记项 |
| `release-payload` | `dist/` 的发布载荷数量/体积与 README 记录的口径一致（容差 ±3 MB） |
| `portable-modules` | 可移植模块与单元测试里的 Windows API **只出现在 `#ifdef _WIN32` 分支内** —— 这是 Linux CI 能跑 C++ 测试的前提 |
| `ci-entrypoints` | `.workflow/*.yml`、`.githooks/pre-commit`、`ci*.bat` 里引用的 `tools/`、`tests/` 路径都真实存在 |
| `no-credentials` | 被跟踪的文本文件里没有硬编码的令牌/私钥 —— 公开仓库里一条 32 位 hex 的密钥进了历史就只能吊销重发，所以推之前机械拦一道 |

**新鲜度 / 文档**

| 门禁 | 断言 |
|---|---|
| `third-party-notices` | `THIRD_PARTY_NOTICES.md` 与当前依赖一致，且含 GPL v3 义务说明 |
| `doc-links` | 所有 Markdown 的相对链接都能解析到真实文件 |
| `docs-index` | `docs/README.md` 收录了 `docs/` 下每一份文档 |
| `readme-tree` | README 的目录结构里列出的文件都真实存在 |

> 缺 `vendor/` 或 `dist/` 时，相关门禁记 **SKIP**（不是 FAIL）—— 托管 CI 上不会有
> `vendor/`，那里跑门禁是为了抓文档漂移，不该因为"没下依赖"而报红。
> `tests/python/test_verify.py` 里有一条用例把 ROOT 指到空目录跑全部门禁，
> 断言没有门禁会抛 `Skip`/`Fail` 之外的异常（生成器用 `SystemExit` 报错，
> 不接住会把整个门禁进程掀掉 —— 这个坑修过）。

### 怎么加一条新门禁

```python
@gate("my-gate", "一句话说明它断言什么")
def g_my_gate(verbose):
    ...
    if 有问题:
        raise Fail("具体哪里不一致，要能直接照着修")
    if 前提不满足:
        raise Skip("为什么跳过（例如 dist/ 不存在）")
    return "通过时打出的证据（带上数字）"
```

两条要求：① 失败信息要**直接指出哪两处不一致**，不要只说"校验失败"；
② 能核出数字的就给数字（"13 项边界一致"比"通过"有用得多）。

---

## 5. CI 编排（`tools/ci.py`）

一个入口，两种环境：

```bat
ci.bat                                    :: Windows 一键（等价于下一行）
F:\Python314\python.exe tools\ci.py        :: 本地：能跑的都跑
python3 tools/ci.py --profile ci           :: 云端：只跑可移植阶段
F:\Python314\python.exe tools\ci.py --list        :: 列出阶段与可移植性
F:\Python314\python.exe tools\ci.py --only gates,cpp-unit
```

阶段自己做前提判定：**满足就跑，不满足记 `[SKIP]` 并写明原因**（跳过 ≠ 通过）。
所以同一份编排在本机能跑全量、在 Linux 容器里能跑可移植子集，而且"哪一段在哪个环境跑不了"
是显式可见的 —— 不会把"没跑到"混进"绿了"。

| 阶段 | 可移植 | 前提 |
|---|---|---|
| `gates` | 是 | 无 |
| `py-unit` | 是 | Python 标准库 |
| `cpp-unit` | 是 | g++ / MinGW（**不需要 vendor/**） |
| `smoke` / `build` / `release` | 否 | Windows + vendor/ |
| `selftest` / `cli` | 否 | Windows + dist/ |

### 为什么 CI 只有一半是自动的（重要，别当成遗漏）

Gitee Go 的托管构建环境是 **CentOS 8.3 / Ubuntu 20.04**（官方插件的 `build@python` /
`build@gcc`），**没有 Windows 构建环境**。而这个播放器是 Windows 独占的：MinGW 工具链 +
SDL3/FFmpeg 的 Windows 二进制 + Winsock2。因此：

- `.workflow/pluginplayer-ci.yml` **只跑可移植子集**（`gates` + `py-unit` + `cpp-unit`），
  这一点写在该文件开头；
- 内核/插件的构建、冒烟、自检、发布构建**必须在 Windows 上跑**：本地 `ci.bat`，
  或自建一台 Windows 构建机（Gitee Go 支持自定义构建机）。

**诚实标注**：那个流水线文件按官方文档的 schema 编写，但**未在 Gitee Go 上实跑验证过**
（开通 Gitee Go 要在网页端操作并绑定手机号，无法从仓库侧触发）。另外 Gitee Go 通常需要
通过网页"新建流水线"才会接管 `.workflow/` 下的配置 —— 直接把文件提交上去不保证自动注册。

可移植子集能成立，靠的是 `portable-modules` 门禁：本机装不了 Linux，
"能在 Linux 上编"这件事跑一遍证明不了，只能用静态检查把那个前提钉住。

---

## 6. 提交前该跑什么

最省事的做法是一条命令：

```bat
ci.bat                                          :: 门禁 → 两种单测 → 冒烟 → 自检 → 色彩 → CLI → 发布（约 2 分钟）
```

想快就只跑门禁（<1 秒、不需要构建产物、也不需要 vendor/）：

```bat
F:\Python314\python.exe tools\verify.py
```

按改动类型挑着跑：

| 改了什么 | 至少跑什么 |
|---|---|
| 文档 / 注释 | `tools/verify.py` |
| 数值边界 / CLI | `tools/verify.py` + `python -m unittest discover -s tests/python` |
| `util` / `config` / `eventbus` / `registry` | `build.py --unit` |
| 插件 / 内核 | `build.py --smoke` + `pluginplayer selftest` |
| 打包 / 依赖 | `build.py --release` + `tools/verify.py` |

**不必**每次跑全量发布构建（`build.py --release` 约 46 秒且会清空 `dist/`）。
只在要出发布物或验证打包口径时跑。

也可以装成本地钩子（**可选**，只影响你自己的仓库配置）：

```bat
git config core.hooksPath .githooks
```

之后每次 `git commit` 会自动跑 `tools/verify.py`（只跑静态门禁，不构建、不联网）。
这与 `deepseek-harness-master` 用 lefthook 做 pre-commit 是同一个思路：
本地钩子只放"快而必要"的检查，完整矩阵交给别处。不想用就 `git config --unset core.hooksPath`。

---

## 6b. 怎么量播放性能（**别用 `--selftest`**）

`--selftest` **不能**用来量渲染/上传路径的性能，因为它自己会污染口径：它要为证据图
把每帧物化成 RGBA（CPU 逐像素转换），于是量到的是"物化开销"而不是播放管线。
本仓库在这上面栽过两次（见 README 验证结果 §17），所以性能改进一律按下述方法量。

**方法（每一步都不可省）**

1. **真实播放**：`pluginplayer --hidden <媒体>`，不要 `--selftest`。
2. **控制通道采样**：连上 RPC，周期性 `player.stats`，取 `decoded / displayed / dropped`
   **增量**（不是绝对值 —— 连接握手期间已经放过一段，绝对值里混着启动期的数）。
3. **读进程 CPU 时间**：`GetProcessTimes` 的 kernel+user，纯 ctypes，不依赖 psutil。
4. **归一化成 `cpu/帧 = cpu增量 / displayed增量`** —— 这个指标能抵消时钟快慢、
   窗口模式、采样窗口长短带来的差异，是唯一可跨次比较的数。
5. **同一份二进制上做 A/B**：用环境变量切路径，不要拿两次不同构建的数字比。
   - `PP_NO_NV12=1` 强制 RGBA 上传路径 ↔ 默认 NV12
   - `PP_NO_HWDEC=1` 强制软解 ↔ 默认 D3D11VA 硬解
6. **素材要选"能压出差距"的**：用 30 fps 而不是 60 fps 的长片，让 `pumpFrame()` 的
   追帧机制不会被"帧间隔大"悄悄掩盖；并且**至少一个分辨率是明显超配的**（如 4K）。

**读结果时的两个坑**

- `dropped` 只反映"帧晚于主时钟被整批跳过"，**不等于**卡顿本身。要配合 `pumpFrame()`
  的丢帧 WARN 一起看（它给出"距上次取帧的间隔"与队列深度，见 ARCHITECTURE §3a）。
- 中文/UTF-8 输出在 PowerShell 里被重定向会按控制台代码页解码，落盘会乱码 ——
  那是**捕获假象**，不是程序问题。验证子进程的真实字节请直接让脚本自己写文件。

---

## 7. `.bat` / `.cmd` 契约（三条硬约束）

生成这两类文件的代码是 `build.py:make_launcher()`（`run.bat` / `selftest.bat` /
`start-dsh.bat` / `ci.bat`）与 `tools/pp_cli.py:shim_text()`（`pluginplayer.cmd` / `pp.cmd`），
两边都遵守：

> `start-dsh.bat` 是**本项目的启动器**（双击开播放器窗口 / 把媒体文件拖上去即播）：
> 它转发 `run.bat`，启动逻辑只保留一份。它一度被用作 deepseek-harness 的 Web UI
> 启动器（上游那份在 `G:\notpython\deepseek-harness-master\start-dsh.bat`；两个 dsh
> 小工具 `tools/dsh_bind_workspace.py` / `tools/dsh_open_ui.py` 仍留在 `tools/` 里可
> 独立使用），现已按项目所有者要求改回"启动本项目"这一职责。

1. **纯 ASCII + CRLF**。cmd.exe 按当前代码页（本机 GBK）解读 `.bat`，混入中文在非中文
   代码页的机器上会乱码/截断。**`bat-ascii-crlf` 门禁 + `tests/cli_check.py` 都会拦。**
2. **解释器路径必须加引号**（`C:\Program Files\...` 会在空格处拆词），代价是
   `py -3` 这类"命令 + 参数"不能整体加引号 —— 所以垫片拆成 `PP_PY` + `PP_PYARG`。
3. **`PP_ROOT` 一律带尾部分隔符**，脚本里固定写 `"%PP_ROOT%tools\pp_cli.py"`，
   这样绝对路径版与 `%~dp0` 版能共用同一份脚本体（`cli_check.py` 断言两份除 root
   赋值行外逐字节一致）。

`.gitattributes` 把 `.bat`/`.cmd` 标为 `-text`，即 git 完全不参与它们的行尾转换 ——
`core.autocrlf=true` 也管不到，入库字节 = 检出字节。

---

## 8. 改数值边界时（最容易漏的一步）

数值边界写在**两处**，改的时候必须一起改，否则 `arg-bounds-align` 门禁会失败
（这正是它存在的意义）：

| 参数 | C++ 侧 | CLI 侧 |
|---|---|---|
| `--size` | `src/core/app.h` 的 `kMinWindowSize` / `kMaxWindowSize` | `tools/pp_cli.py` 的 `MIN_WIN` / `MAX_WIN` |
| `--frames` | `src/core/main.cpp` 的 `kMaxFrames` | `MIN_FRAMES` / `MAX_FRAMES` |
| `--volume` | `main.cpp` 的范围判断 | `MIN_VOLUME` / `MAX_VOLUME` |
| `--log-level` | `main.cpp` 的报错文案里的区间 | `MIN_LOG_LEVEL` / `MAX_LOG_LEVEL` |
| `--reload` | `main.cpp` 的报错文案里的区间 | `MIN_RELOAD` / `MAX_RELOAD` |
| `--rpc-port` | `main.cpp` 的报错文案里的区间 | `MAX_PORT` |
| `--rpc-token` | `main.cpp` 的 `size() < 16` | `MIN_TOKEN_LEN` |
| 报文/并发上限 | `src/core/control.h` 的 `kMaxLineBytes` / `kMaxClients` | `SERVER_MAX_LINE_BYTES` / `SERVER_MAX_CLIENTS` |

CLI 侧只在自己有而 C++ 没有时才用裸字面量（`--start`、`speed`），那些已单独注明。

---

## 9. 常见问题

| 现象 | 原因 | 修法 |
|---|---|---|
| `找不到编译器` | 本机路径不同 | `set PP_GXX=<MinGW安装目录>\bin\g++.exe` |
| `缺少依赖目录 vendor/...` | 没跑 `fetch_deps.py`，或归档不在候选目录 | 跑 `tools/fetch_deps.py --src <含归档的目录>` |
| 运行产物报 `0xC0000135` | 找不到 DLL（`build/*.exe` 与 `dist/` 不同目录） | 用 `build.py` 跑（它补了 PATH），或把 `dist/` 加进 PATH |
| `doctor` 报「控制通道令牌不存在」 | 播放器没启动过 | 正常，`WARN` 不阻塞 |
| 门禁 `release-payload` 失败 | `dist/` 没按 `--release` 重建，或改了打包内容 | `build.py --release`；改了口径就同步 README 的数字 |
| 门禁 `doc-links` 失败 | 文档里链接到已删除/改名/尚未创建的文件 | 补文件或改链接（例如 `LICENSE` 未添加时会提示） |
| 门禁 `third-party-notices` 失败 | 依赖变了，生成物没跟上 | `tools/gen_third_party_notices.py` |

> 本机环境提醒：这个会话里 Bash 工具可能缺 coreutils（`ls`/`head`/`wc` 都没有），
> 用 Python 绝对路径做文件系统探查更稳；PowerShell 的 stdout 偶尔被静默吞掉，
> 必要时落盘再读。
