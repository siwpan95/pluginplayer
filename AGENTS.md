# AGENTS.md

PluginPlayer 是一个把**一切功能做成插件**的 Windows 播放器：FFmpeg 9.0.1 解码 + SDL3 渲染音频，
自研 C ABI 插件宿主，另有基于 Cordis 的进程外 JS 插件层。改代码前先读
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)（分层与线程模型是其余一切的前提）；
改文档先读 [docs/README.md](docs/README.md)（文档索引与写法约定）。

本文件是**规则与边界**：哪些约束不能破、改哪类东西要跑什么、哪些目录不要碰。
它是给 AI 助手和协作者共用的 —— `CLAUDE.md` 只是指向这里。

## 仓库布局

```
src/sdk/player_plugin.h   插件 ABI（纯 C，进程内插件的唯一契约）
src/core/                 播放内核 + 插件宿主（player / app / host / eventbus /
                          registry / pluginhost / osd / control / config / util / main）
src/plugins/<name>/       8 个原生插件，各自独立 .cpp → .dll
host-js/                  进程外 JS 插件宿主：src/（Cordis 宿主）+ plugins/*.mjs
examples/hello_plugin/    最小原生插件示例（PLUGIN_GUIDE 逐行讲解）
tests/                    冒烟（smoke_link / smoke_audio）、插件 API 回归、CLI 回归（cli_check.py）、
                          unit/（C++ 单元测试 + 零依赖 harness）、python/（标准库 unittest）
tools/                    fetch_deps（依赖）、rpc_client（协议测试客户端）、
                          pp_cli（命令行接口）、verify（18 条机械门禁）、ci（CI 编排）、
                          gitee_release（建仓/转公开/发发行版）、
                          gen_third_party_notices（第三方声明生成器）、
                          dsh_bind_workspace（把目录注册成 dsh 工作区）、
                          dsh_open_ui（dsh 就绪后自动打开浏览器）
docs/                     文档（索引在 docs/README.md）
.workflow/                Gitee Go 流水线（只跑可移植子集，原因写在文件头）
.githooks/pre-commit      可选本地钩子（core.hooksPath）
ci.bat / build.py         一键 CI / 构建脚本（.bat 由 build.py 生成，勿手改）
vendor/                   第三方依赖的头文件与导入库（不入库，fetch_deps.py 生成）
dist/                     构建产物（不入库，build.py 生成）
player/ _extract/ _deps/  参考源码与依赖归档缓存（不入库）
```

## 命令

```bat
F:\Python314\python.exe tools\fetch_deps.py        :: 准备 vendor/（首次必须）
F:\Python314\python.exe build.py                   :: 编译内核 + 插件 + 打包 dist/
F:\Python314\python.exe build.py --unit            :: C++ 单元测试（不需要 vendor/）
F:\Python314\python.exe build.py --smoke           :: 工具链冒烟
F:\Python314\python.exe build.py --apitest         :: 编译插件 API 回归插件
F:\Python314\python.exe build.py --release         :: 出发布物（干净重建 + 清理 + 清单）
F:\Python314\python.exe tools\verify.py            :: 18 条机械门禁（<1 秒，不需构建产物）
F:\Python314\python.exe tools\verify.py --selfcheck :: 对每条门禁故意破坏，断言它必须报红
F:\Python314\python.exe tools\ci.py                :: CI 编排（分阶段，前提缺失记 SKIP）
F:\Python314\python.exe tests\cli_check.py         :: CLI 回归（端到端）
F:\Python314\python.exe -m unittest discover -s tests\python   :: Python 单元测试
ci.bat                                             :: 一键：门禁 → 单测 → 冒烟 → 自检 → CLI
F:\Python314\python.exe tools\pp_cli.py install-cli  :: 装好后可用 `pluginplayer <子命令>`
```

装过 CLI 之后：`pluginplayer doctor` / `selftest` / `build --release` / `status` / `cmd <名>` /
`events` / `plugins` / `manifest`。全量命令与退出码见 [docs/CLI.md](docs/CLI.md)。

## 约定（这些都有可执行检查，别只当风格建议）

**架构**

- **新增行为走插件扩展点，不改内核。** 改 `player.cpp` / `app.cpp` 属于最后手段，
  且必须同步更新 `docs/ARCHITECTURE.md`。
- **插件不链接 FFmpeg / SDL3。** 一切能力经 `PP_HostApi` 提供 —— 插件因此很小，
  也不存在与宿主争 DLL 版本的问题。
- **插件的全部回调都在主线程执行。** 所以插件**不需要**为自己的状态加锁，
  但**绝不能阻塞**（会卡住整个播放）。
- **控制通道请求在主线程串行执行。** 工作线程只做"收报文 → 入队 → 等结果 → 回包"，
  绝不让 RPC 线程直接碰 `Player` / `Host` 的非原子状态（历史上这里是真实的数据竞争）。
- **安全策略由宿主强制，不由插件自觉。** 插件是不可信代码：写入白名单、网络输入默认拒绝、
  令牌认证、报文与并发上限，全部在宿主侧。默认开，便利选项显式开（`--allow-*`）。
- **ABI 靠严格相等兜底。** `PP_MediaInfo` / `PP_PlaybackStats` / `PP_EventCtx` / `PP_Plugin`
  没有 `struct_size`，加字段必须递增 `PP_ABI_VERSION` 并重编**全部**插件
  （规则写在 `src/sdk/player_plugin.h` 顶部）。

**数值与参数**

- **数值边界写在两处**（C++ 与 `tools/pp_cli.py`），改一处必须改另一处 ——
  `arg-bounds-align` 门禁会拦。完整对照表见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#8-改数值边界时最容易漏的一步)。
- **参数先校验、后启动。** 非法值在起进程之前就退 2，不带越界值继续跑。
- **CLI 不做二次实现。** `build` / `selftest` 只转发，逻辑留在 `build.py` 与
  `player.exe`，并原样透传退出码（不许压成 1）。

**代码**

- **Python 工具零第三方依赖**（只用标准库）。插件侧同理：`audio_viz` 的 FFT 是自实现。
- **测试也不用第三方框架。** C++ 用 `tests/unit/harness.h`（百来行，看得懂），
  Python 用标准库 `unittest`。加依赖要能说清"它删掉了多少自己维护的代码"。
- **注释写契约，不写推理过程。** 不要复述代码已经说清的事实；踩过坑的地方写清"为什么这样"。
- **宁可用可执行的检查，不要用散文约束。** 发现某条规则反复被违反，就加一条门禁；
  加了门禁就**必须同时加一条 `--selfcheck` 破坏用例**（否则没人知道它拦不拦得住，
  `tests/python/test_verify.py` 里有断言强制这一点）。

**可移植性（CI 的命门）**

- `src/core/{util,config,eventbus,registry}.cpp` 与 `tests/unit/` 是**可移植的**：
  Windows API 只能出现在 `#ifdef _WIN32` 分支里，非 Windows 分支要给出可编译的实现
  （`sha256FileHex` 是例外：非 Windows 上直接返回 false，调用方据此跳过校验）。
  **`portable-modules` 门禁会拦** —— 破坏了它，Linux CI 会在编译期炸在一个看起来
  无关的地方。
- 这四份代码是"能在 Linux 上跑 C++ 单元测试"的唯一基础。托管 CI 没有 Windows 环境，
  所以**不要把内核逻辑塞进这四个模块**（它们只该是纯逻辑与安全策略）。
- 其余模块（`player` / `app` / `host` / `pluginhost` / `osd` / `control`）是 Windows 独占的，
  不要试图让它们跨平台编译 —— 那会把 `#ifdef` 撒满整个内核。

**文本与文件**

- **`.bat` / `.cmd` 必须纯 ASCII + CRLF**，解释器路径加引号，`PP_ROOT` 带尾部分隔符。
  三条约束及其原因见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#7-bat--cmd-契约三条硬约束)。
- **生成物只能由生成器写**：`THIRD_PARTY_NOTICES.md` 由 `tools/gen_third_party_notices.py` 生成，
  手改会被 `third-party-notices` 门禁判过期。`dist/`、`run.bat`、`selftest.bat`、
  `start-dsh.bat`、`ci.bat`、`pluginplayer.cmd`、`pp.cmd` 同理。
- **已知限制要如实写进 README 的「已知限制」**，包括"没做"和"做得不彻底"的部分。
  宁可写「测试仍然很薄」，也不要让读者误以为它是完整的。

## 不要做的事

- **不要改 `dist/` 里的任何文件** —— 它是构建产物，改了会被下次构建覆盖，也骗过不了
  `plugin-manifest` 门禁。要改行为就改源码后重建。
- **不要手工改 `THIRD_PARTY_NOTICES.md`**（见上）。
- **不要 `git add` `dist/` / `vendor/` / `player/` / `_extract/` / `_deps/`**，
  也不要用 `-f` 强加 —— `.gitignore` 排除它们是有意的。
- **不要提交 `config/rpc-token.txt`、`logs/`、`selftest_out*/`、`test_assets/`**。
- **不要把断言放宽来让测试变绿。** 例如 `plugin_hot_reload` 刻意断言
  "订阅/命令/服务/帧处理器数量完全不变"，它曾经只会断言 `> 0`，等于永远为绿；
  `decode_and_display` 的下限、`cli_check.py` 里"根本没走到启动那一步"都是同一类加严。
  真要改口径，说明理由并同步文档。
- **不要用中文/emoji/非 GBK 字符写 `.bat`**，也不要在 CLI 的帮助文本里用 emoji
  （非 GBK 字符在 cmd 里会变 `?`）。
- **不要在没跑过的情况下声称"已验证"**。结论必须配可复现证据。
- **不要把 Windows API 写进可移植模块的 `#ifdef` 之外**（见上「可移植性」）——
  `portable-modules` 门禁会拦，但更该知道的是：那会让 Linux CI 在编译期失败。
- **不要让 CI 的"绿"产生错觉。** `.workflow/` 里的流水线**只覆盖可移植子集**；
  Windows 侧（构建、冒烟、自检、发布）没有任何自动化。改了内核/插件却说"CI 过了"，
  等于什么都没说 —— 必须说明是在**哪台机器上**跑的 `ci.bat`。

## 改动的验证门槛

| 改了什么 | 至少跑什么 |
|---|---|
| 文档 / 注释 | `tools/verify.py` |
| 数值边界 | `tools/verify.py`（会指出哪一项不一致）+ `tests/python/test_pp_cli.py` |
| CLI（`tools/pp_cli.py`） | `tools/verify.py` + `tests/python/` + `tests/cli_check.py`（控制通道用例需运行中的实例） |
| `util` / `config` / `eventbus` / `registry` | `build.py --unit` + `tools/verify.py`（`portable-modules` 会拦 WinAPI 越界） |
| 插件 / 内核 | `build.py --smoke` + `pluginplayer selftest` + `tools/verify.py` |
| 门禁本身（`tools/verify.py`） | `tools/verify.py --selfcheck`（新门禁必须配套破坏用例）+ `tests/python/test_verify.py` |
| 依赖版本 | `fetch_deps.py --refresh` → `build.py --release` → `gen_third_party_notices.py` → `verify.py` |
| 打包内容 | `build.py --release` + `verify.py`（`release-payload` 会核对 17 个文件 / 127 MB 口径） |

不确定就跑 `ci.bat`（约 60 秒，覆盖上面除发布构建之外的全部）。

**不必**每次跑全量发布构建：`build.py --release` 约 46 秒且会清空 `dist/`。
只在要出发布物或验证打包口径时跑。`tools/verify.py` 是常态入口（<1 秒、不需构建产物、
不需 vendor/）。

## 汇报要求

- 说清**改了什么、为什么、怎么验证的**，附上真实命令与输出。
- 分清「已验证 / 未能验证 / 未修」三态。未修项要登记到 README 的「已知限制」并写明**为什么不修**。
- 门禁报 FAIL 时照它给的差异去修，不要顺手把门禁改松。
