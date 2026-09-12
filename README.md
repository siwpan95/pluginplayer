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

:: 3) 播放（把视频拖到 run.bat 上，或：）
dist\pluginplayer.exe 你的视频.mp4

:: 4) 一键自检（生成测试视频 → 跑通全链路 → 输出 JSON 报告）
selftest.bat

:: 5) 启动 JS 插件宿主（可选，需要先给播放器开控制通道）
dist\pluginplayer.exe --rpc-port 7911 你的视频.mp4
cd host-js && npm install && node src\index.mjs --port 7911
::   浏览器打开 http://127.0.0.1:7912/ 即是远程控制页
```

播放中快捷键：`SPACE` 播放/暂停、`←/→` ±5s（Shift ±30s）、`↑/↓` 音量、
`0-9` 跳百分比、`[ / ]` 变速、`F` 全屏、`S` 截图、`H` 帮助、`TAB` 列命令、`P` 插件报告。

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
├─ tools/fetch_deps.py                 依赖准备；rpc_client.py 控制通道测试客户端
├─ build.py                            构建脚本（不依赖 cmake/make）
├─ docs/ARCHITECTURE.md                线程模型 / A-V 同步 / 协议 / 踩坑记录
├─ docs/PLUGIN_GUIDE.md                两种插件的开发指南（含可编译示例）
└─ dist/                               产物：exe(450KB) + DLL + plugins/ + config/
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

### 1. 全链路自检 `selftest.bat`

```
[19:0x][INFO ] [PASS] sdl_init              — SDL3 video driver=windows
[INFO ] [PASS] plugins_loaded        — 加载 5 个插件，失败 0 个
[INFO ] [PASS] services_registered   — 服务 2 个
[INFO ] [PASS] commands_registered   — 命令 15 个
[INFO ] [PASS] open_media            — mp4 1280x720 30.00 fps dur=8.00 s codec=h264/aac
[INFO ] [PASS] decode_and_display    — 显示 90 帧（目标 90），被插件丢弃 0 帧
[INFO ] [PASS] seek                  — 目标 4.80 s，seek 后位置 4.99 s
[INFO ] [PASS] plugin_commands       — snapshot -> 0  stats -> 0  effect.toggle -> 0
[INFO ] [PASS] render_and_capture    — 渲染帧已抓屏保存
[INFO ] [PASS] plugin_hot_reload     — 重载后 订阅 4->4, 命令 15->15, 服务 2->2, 帧处理器 2->2
[INFO ] ==== 自检结束: 10/10 项通过，用时 3.90 s ====
```

产物：`dist/selftest_out/` 下的 `selftest_report.json`、`frame_00{1,2,3}.png`
（解码出的原始帧）、`snapshot_0001.png`（**插件**经 `acquire_frame` + 宿主 PNG 写出保存）、
`render_output.bmp`（SDL 渲染抓屏）。

### 2. 控制通道 `tools/rpc_client.py`

```
=== 控制通道: 19/19 通过 ===
（ping / player.info / player.stats / service.list / plugin.list /
  pause / play / seek / volume / speed / command.list /
  command.invoke × 4 / 错误处理 × 1 / 事件推送 app.tick）
```

### 3. JS 插件宿主

```
[host-js] 已装载 JS 插件: event-log
[host-js] 已装载 JS 插件: auto-resume
[host-js] 已装载 JS 插件: web-remote
[host-js] JS 插件 3 个（失败 0）  控制通道: 已连接 abi=1  position=3.81s paused=0  fps=30.0
[host-js] 服务计数: 请求 8 / 事件 33
[host-js] 收到 --exit-after 8s，按逆序卸载 4 个 Fiber…
[host-js] 已全部卸载
```

`.state/js-event-log.json` 记录了 `{app.tick:31, media.eof:1, player.completed:1}`，
`.state/positions.json` 记录了续播位置 —— 证明事件与状态双向打通。
浏览器访问控制页返回 `http=200 bytes=5693`，`POST /api/action pause` 与
`POST /api/action command {name:"effect.cycle"}` 均返回 `ok:true`，
内核侧同时打印 `[plugin] 效果轮转: gray`（**JS 成功驱动了 C 插件**）。

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
| 验证方式 | 手工点界面 | `--selftest` 无人值守，产出 JSON 报告 + 帧 PNG |

---

## 已知限制

- 只实现 Windows（Winsock2 控制通道、MinGW 工具链）。内核里除 `util.cpp` 的路径/目录部分外基本可移植。
- 没有做硬件加速解码（`d3d11va`/`dxva2`），4K 高码率可能吃 CPU。
- 不解析字幕（`.srt`/`.ass`），只画 OSD；字幕可作为下一个插件接入（`PP_TOPIC_VIDEO_FRAME` + OSD 已够用）。
- 无播放列表 UI；用控制通道或 `--out`/命令扩展。
- JS 侧 `npm install` 需要网络（`cordis` 与 `@cordisjs/plugin-loader` 从 npm 拉取）。
- 单文件播放；跨文件续播由 `auto-resume` 插件负责但不做自动切集。
