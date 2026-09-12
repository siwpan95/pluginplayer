# PluginPlayer — 插件版播放器

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
| `--allow-network-input` | 允许打开 `http/rtsp/UNC` 等网络输入（**默认拒绝**） |
| `--size WxH` | 初始窗口尺寸（默认 1280x720，范围 160..8192） |
| `--selftest --frames N --out DIR` | 无人值守自检 |

所有数值参数都会做严格校验，非法值直接报错退出（`exit 2`），不会带着越界值继续跑。

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
├─ tools/fetch_deps.py                 依赖准备；rpc_client.py 控制通道测试客户端（带令牌）
├─ build.py                            构建脚本（不依赖 cmake/make；--smoke / --apitest）
├─ .gitignore                          排除构建产物、第三方依赖、运行期产物与令牌文件
├─ docs/ARCHITECTURE.md                线程模型 / A-V 同步 / 协议 / 安全模型 / 踩坑记录
├─ docs/PLUGIN_GUIDE.md                两种插件的开发指南（含可编译示例）
└─ dist/                               产物：exe(~470KB) + DLL + plugins/ + config/
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
| 控制通道压力：7 并发 × 8 秒 | 3392 次请求全部成功，**423 req/s**，p50 16.6 ms / p95 17.4 ms，无崩溃无异常读数

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
| 验证方式 | 手工点界面 | `--selftest` 无人值守产出 JSON 报告 + 帧 PNG；另有 `build.py --smoke` 冒烟与 `--apitest` 插件 API 回归 |
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

- **测试仍然很薄**：只有 2 个冒烟程序 + 1 个插件 API 回归插件，**没有单元测试框架**，
  也没有 CI。`build.py --smoke` / `--apitest` 是最小入口，不是完整测试网。
- **RPC 延迟受主循环帧期约束**：请求在主线程串行执行，60fps 下 p50 ≈ 16.6 ms。
  如果将来需要低延迟控制，得改成"主线程只做状态快照、慢操作异步化"。
- **插件无签名/哈希校验**：`plugins/*.dll` 里放什么就执行什么；加载也未使用
  `LOAD_LIBRARY_SEARCH_*` 加固标志（插件自身的依赖仍按默认搜索顺序解析）。
- **依赖组件的 CVE 未扫描**：随包的 FFmpeg 9.0.1 / SDL3 3.4.16 没有做过漏洞比对。
  不可信媒体文件的解码面是目前最大的残余风险。
- **`openMedia` 没有 `interrupt_callback`**：打开损坏/超大/网络流输入时可能阻塞较久
  （控制通道会因为主线程被占而暂时不可用，10 秒后返回"主线程繁忙"）。
- **`config/pluginplayer.ini` 的键按槽位号命名**（`plugin.<槽位>.<键>`），而槽位号是
  DLL 名字母序 —— **增删插件会让既有配置整体错位**。目前用 `--write-dir` 等命令行
  参数绕开；彻底修法是改成按插件名索引。
- **网页控制页默认关闭**：想用必须显式开启（`PP_WEB_REMOTE=1`）。这是刻意的，
  因为它能驱动任意插件命令。
- 控制通道加认证后，**所有**客户端都必须带令牌（`tools/rpc_client.py` 与
  `host-js` 已内置；自己写的脚本要看 `control.h` 里的协议说明）。
