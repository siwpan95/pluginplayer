# PluginPlayer 播放体验增强规划（v1 草案）

> 状态：**P0 全部闭环（B1–B5，2026-09-12）；P1/P2 未开始**
> 基线：`G:\local\pluginplayer` @ `af6bacc`（工作树含未提交产物目录 `_dist_baseline_backup` / `_vendor_backup_20260912`）
> 诊断日期：2026-09-12
> 关联约束：[AGENTS.md](AGENTS.md)（仓库规则与验证门槛）、[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)、[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)
> 实施进度与实测数字：[README.md 「验证结果」§14–§19](README.md)（本文件保留**规划与决策过程**，逐批记录含"当时判断错了什么"）

---

## 0. 结论卡（TL;DR）

**当前项目的问题不在工程纪律，而在两件事：① 一条被自己"证据图"掩盖的渲染/验证缺陷；② 画质与体验链路的技术选型只做到"能播"。**

| # | 结论 | 状态 | 关键依据 |
|---|---|---|---|
| 1 | **自检的"可视证据图" `render_output.bmp` 里根本没有视频画面** —— 86.62% 的像素恰好等于窗口清屏色 `RGB(16,16,20)`，纯黑占 0.00%，非背景部分只有 OSD。也就是说这张证据图从启用以来就没有证明过"画面出得来" | ~~已实测确认~~ → **已修复（B1，2026-09-12）**，同一指标 86.60% → 0.39% | 像素直方图（§1.3）+ `app.cpp:352` 与 `app.cpp:700` 的交互；修复记录见 §4 P0-1 |
| 2 | OSD 用 SDL 调试点阵字体 + `sanitize()` 把非 ASCII 全部降级成 `?`，**中文界面信息 100% 不可读** | **已实测确认** | 抓屏截图（§1.4）+ `osd.cpp:44-66` + `player_plugin.h:274` |
| 3 | OSD 默认**压在画面上**：16:9 窗口播 16:9 素材时没有黑边可避让，而两个 OSD 面板正好盖住左上角（素材水印所在处） | **已实测确认** | §1.4 截图与几何计算（§2.5） |
| 4 | 渲染/解码链路**没有色彩管理**（已修，B3）、**没有硬件解码**（P0-4a 已接入，2026-09-12）、缩放只剩 SDL 的双线性采样（待 P2-1） | 色彩：**已修复（B3）**；硬解：**已接入（B4a）**；上传路径：**NV12 已启用（B4d）**，4K30 满帧零丢帧 | `player.cpp` 色彩段；`openVideoCodec()` 的 D3D11VA 分支；README §16 |
| 5 | "黑屏无画面"：**已复现并修复**。真因不是时序竞争，而是 `Player::open()` 漏复位 `demuxFinished_`/`drainSent_` —— 同一进程里换片时 demux 线程一个包都不读，`finished()` 当场为真把主时钟冻在 0 ⇒ **必然永久黑屏**（控制通道 `player.open`、插件 `open_media` 都会命中） | **已修复（B2，2026-09-12）**；曾用注释掉两行复位复现 | 修复记录与证据见 §4 P0-2；回归用例 `--selftest` 的 `reopen_media` |
| 6 | 依赖侧**已经具备**升级条件：随包 SDL3 3.4.16 支持 NV12 纹理、YCbCr 矩阵、D3D11 外部设备复用；FFmpeg 9.0.1 头文件带 `BindFlags`。零拷贝硬解路线可行 | **已核验** | `vendor/sdl3/include/SDL3/SDL_render.h:565,814`、`SDL_pixels.h:670,1066`、`vendor/ffmpeg/include/libavutil/hwcontext_d3d11va.h:103,166,174` |

**建议路线**：P0 先修"看不见/不可信"，P1 补"看得对"（色彩 + 中文 + 鼠标 + 字幕），P2 补"看得好/好用"（GPU 着色器画质链、播放列表、HDR）。
**架构红线**：内核只升级**能力通道**，功能仍然**全部做成插件**（Cordis 模型不破）。

---

## 1. 诊断的证据基础（可复现）

以下所有数据都是本次实跑得到，命令与原始输出可复核。

### 1.1 环境与实际媒体

用户日志 `dist/logs/pluginplayer.log` 显示的真实播放对象（**文件仍在**）：

```
G:\repos\bilibili-helper-py\output\第二十届FIRST青年电影展FIRST PIONEER先锋创作年度入围｜《香港糖果盒》预告片.mp4
mov,mp4,m4a,3gp,3g2,mj2  852x480  24.00 fps  dur=104.87 s  h264/aac  471 kbps  1ch/48kHz
```

### 1.2 自检实跑（结果 PASS，但可看到真问题）

```powershell
# 注意：必须用「调用运算符」传参。Start-Process -ArgumentList 会把中文路径
# 按 ANSI 转换而破坏它（实测 open_media 直接 FAIL，路径被截断）。
Set-Location G:\local\pluginplayer\dist
& .\pluginplayer.exe --selftest --frames 60 --rpc-port 0 --out <OUT> "<上述中文路径 mp4>"
```

```json
{ "result": "PASS", "pass": 10, "total": 10, "elapsed_sec": 3.422,
  "playback": { "displayed": 65, "decoded": 105, "dropped": 24,
                "position": 62.987, "master_clock": 62.987, "video_clock": 62.958 },
  "steps": [ { "name": "plugin_commands", "ok": true,
               "detail": "snapshot <OUT> -> -1  stats -> 0  effect.toggle gray -> 0" },
             { "name": "render_and_capture", "ok": true,
               "detail": "渲染帧已抓屏保存: <OUT>/render_output.bmp (1280x720)" } ] }
```

三条从这份 PASS 里读出来的**反例**：

- `snapshot -> -1`：命令**失败**（写入白名单外，被宿主拒绝），但 `plugin_commands` 仍判 **PASS** —— 该步只断言"命令被调用过"（`app.cpp:744`），不校验返回码。README 里写的 `snapshot -> 0` 是因为当初把 `--out` 放在 `dist/` 内。
- `render_and_capture` 只断言 `surf != nullptr`（`app.cpp:781`），**从不校验抓到的图里有没有画面** —— 见 §1.3，确实没有。
- `dropped: 24 / decoded: 105`（≈23%）：这是自检"无节拍紧循环"的产物，**不代表真实播放丢帧率**，引用时须注明。

### 1.3 决定性的像素直方图 —— 证据图里没有画面

对 `render_output.bmp`（1280x720，自检声称的"渲染帧已抓屏保存"）做精确颜色直方图：

```
size=1280x720 total_px=921600
  RGB(16, 16, 20)   798301   86.62%   <-- 恰好是 app.cpp:402 的 SDL_SetRenderDrawColor 清屏色
  RGB(7,  7,  8)     56399    6.12%   <-- 半透明 OSD 面板 (alpha=170) 与背景混合的结果
  RGB(6,  6,  7)     45356    4.92%   <-- 同上
  RGB(90, 200, 255)   4488    0.49%   <-- OSD 文字高亮色
  ...（其余均为 OSD 颜色）
纯黑(0,0,0) 占比: 0.00%
采样点（避开 OSD）：(640,360)/(320,360)/(960,240)/(640,180)/(40,600) 全部 = RGB(16,16,20)
```

**推理**：素材 852x480 与窗口 1280x720 **同为 16:9**，`app.cpp:408-425` 会把画面矩形算成接近满窗（实测 `dst ≈ {x:1,y:0,w:1278,h:720}`）——即整个窗口都该是画面。但窗口中心的采样点拿到的是**恰好等于清屏色**的值。若画面被画上去，YUV 有限范围的黑场转出来是 `RGB(0,0,0)` 附近而不是 `(16,16,20)`；纯黑占比 0.00% 说明连画面里的黑都没有。

**根因（读写路径交叉确认）**：

```cpp
// app.cpp:345-383  pumpAndUpload()
auto frame = player_.currentFrame();
if (!frame) return;
if (frame->index == lastUploadedIndex_) return;   // ← 352：索引没变就直接返回，tex_ 永不创建
...
SDL_UpdateTexture(tex_, nullptr, frame->rgba.data(), frame->pitch);
```

```cpp
// app.cpp:696-701  selftest 的 seek 循环：已经把这个 index 记掉了
if (f && f->index != lastUploadedIndex_) { lastUploadedIndex_ = f->index; got++; }
// ...
// app.cpp:752  随后才调用 pumpAndUpload()
pumpAndUpload();
```

seek 循环把 `lastUploadedIndex_` 更新到"当前帧"，紧接着 `pumpAndUpload()` 里 `pumpFrame()` 因为下一帧的 `pts > master + 4ms`（`player.cpp:841`）拿不到新帧，`currentFrame()` 还是同一帧 → **`index` 相等 → 直接 return → `tex_` 从未创建 → 抓屏里只有背景与 OSD**。

**可证伪的预测**：在 `app.cpp:752` 的 `pumpAndUpload()` 之前插一行 `lastUploadedIndex_ = -1;`，`render_output.bmp` 应当立刻出现画面。这个实验很小，建议作为 P0-1 的验收实验。

> 影响面：README「验证结果」第 2 节、第 13 节关于"抓屏证据"与"画面零遮挡"的结论，其证据目前不成立；`docs/ARCHITECTURE.md` 第 8 节"渲染几何一律按 `SDL_GetWindowSizeInPixels`"的论证也需要重新出图。

### 1.4 抓屏截图（OSD 中文与遮挡的直接证据）

抓屏图（`_render_output.png`，本次实跑产物）中可以看到：

- 左上信息面板：`?????????????FIRST????????????FIRST PIONEER????????????...` 与 `last=????????.mp4` —— **中文全部变成 `?`**。
- 若把 OSD 关掉（`O` 键），这块区域是素材自带的水印 —— 也就是说 **OSD 面板正好盖住了素材的左上角内容**。
- 底部两条半透明带（`RGB(7,7,8)` / `RGB(6,6,7)`）就是 `osd.panel` 的 48px 进度区与 FFT 面板底板。

---

## 2. 问题清单（按你勾选的五个维度）

### 2.1 正确性：黑屏 / 画面不出

**A. 已确认：自检渲染证据失效**（§1.3）。危害不只是"证据图难看"，而是**接下来所有画质增强都无法验证** —— 改完色彩/缩放，你没有任何可信的对比图。**这是画质工作的前置阻塞项。**

**B. "黑屏无画面"——已复现并修复（B2）。** 下面是初版读码时的推断链，**保留原文**，
因为它记录了"方向对了一半"的过程：`finished()` 的语义确实有问题，但我当时把一个
**确定性缺陷**（`open()` 漏复位两个跨媒体标志）误判成了"只在特定时序下触发的竞争"。
真实根因与修复见 §4 P0-2。初版推断原文如下：

> 读码可确定下面这条链路存在，但它只在特定时序下触发，**我没有复现出来**（本次对这个文件自检是 PASS），所以标记为待确认而非结论：

```cpp
// player.cpp:457-467
bool Player::finished() const {
    if (!opened_) return false;
    if (!demuxFinished_.load()) return false;
    if (vq_.count() > 0 || aq_.count() > 0) return false;
    { std::lock_guard<std::mutex> lk(vfMu_); if (!vframes_.empty()) return false; }
    if (aStream_ >= 0 && ring_.avail() > 0) return false;
    return true;                      // ← 「缓冲区此刻为空」被判成「播放结束」
}
```

```cpp
// player.cpp:794-830
double Player::masterClock() {
    if (finished()) { extLastTick_ = now; ...; return extPts_; }   // ← 时钟被冻结
```

```cpp
// player.cpp:834-845
double master = masterClock();
while (!vframes_.empty() && vframes_.front()->pts <= master + 0.004) { pick = ...; }
// ← 时钟一旦冻结在某个值，所有 pts 更大的帧永远取不出来
```

链条：**demux 会把整个文件尽快读进队列并立刻置 `demuxFinished_ = true`**（日志 `18:11:58` 打开、同一秒就打印"demux 到达文件末尾"即为佐证，`player.cpp:406-418`）→ 音频解码比视频快，3 秒音频环形缓冲（`player.cpp:212`）先排空 → 某一刻 `vq_`/`aq_`/`vframes_`/`ring_` **同时为空**（视频解码仍在追，或主线程刚把帧取完）→ `finished()` 变真 → **主时钟冻结 → 之后所有视频帧都"在未来"，永远取不出来 → 画面定格/黑屏**，同时主循环还会提前打 `播放结束`（`app.cpp:535-540`）。

**可判定的复现/排除判据**（P0-2 的第一件事）：

1. 用真实窗口（不要 `--selftest`）播放一个"视频码率明显高于音频、且时长中等"的文件。
2. 开控制通道，`pluginplayer status` 采样 `player.stats`：
   - 若出现 `position` 远小于 `duration`、而日志已打印"播放结束"→ **假设成立**。
   - 若 `decoded` 持续增长但 `shown` 不增长 → 时钟冻结（同一根因）。
3. 加临时日志（`masterClock` 每次进入 `finished()` 分支时打印 `position/duration/各队列深度`）即可一眼锁定。

**修复方向**（无论复现结论如何都应做，因为现在的语义就是错的）：

- `finished()` 改为依赖**显式排空标志**：视频/音频解码线程各自在收到 drain 空包并吐完最后一帧后置位（现在完全没有这个标志，见 `player.cpp:604` 附近的 `isDrain` 分支）；再叠加"主时钟已到达 `duration_`"的条件。
- `masterClock()` **不得**因为 `finished()` 而冻结在"未播完"的值；至少加"`extPts_ >= duration_ - ε` 才允许冻结"。
- 首帧保证：在**首帧上屏之前**不要推进主时钟，或干脆"解出首帧再 `SDL_ResumeAudioStreamDevice`"（`app.cpp:133`），消除开机黑屏窗口。
- 丢帧策略：现在"每轮只保留最后一帧"（`player.cpp:841-845`）在时钟跳跃时会静默跳过大段画面；应改为**有界追赶**（超过阈值报 `WATERFALL`/事件，让插件可感知），并把"追赶"与"正常丢帧"分开计数。

### 2.2 画质：发灰 / 发虚 / 色带

| 现象 | 代码依据 | 原因 |
|---|---|---|
| **发灰、黑位不对** | `player.cpp:553-564`：`sws_getContext(w,h,fmt, w,h,AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,nullptr,nullptr)` | 完全不读 `AVFrame.colorspace / color_range`。**B3 实测修正**：范围这一半其实没错（swscale 对 `yuv420p`/`yuvj420p` 的默认处理是对的，16→0、235→255 实测通过）；真正错的是**矩阵**——素材标注被忽略，SD 素材会被按 BT.601 猜。修法与证据见 §4 P0-3 |
| **发虚** | 同上的 `SWS_BILINEAR`，**且这里的 w/h 相同 ⇒ 这一步根本没缩放**；真正缩放发生在 `app.cpp:421` 的 `SDL_RenderTexture`，采样质量只有 SDL 的双线性（`SDL_render.h:1262`：默认 `SDL_SCALEMODE_LINEAR`） | 852→1278（1.5×）这种非整数上采样用双线性会明显糊；没有 lanczos/bicubic 可选 |
| **色带** | 全仓无 deband/dither | 8bit 素材在平滑渐变处必然出色带，尤其低码率（本素材仅 471 kbps） |
| **每帧 CPU 转换** | `player.cpp:578` `sws_scale` → RGBA；`app.cpp:383` `SDL_UpdateTexture` | RGBA 上传量是 NV12 的 2.67 倍；4K 下 CPU 转换 + 上传成为瓶颈 |
| **无 HDR** | 全仓无 tone mapping | 10bit/HDR 素材会过曝或发灰 |

### 2.3 性能：高分辨率掉帧

- **纯软解**：全仓 grep 无 `AVHWDeviceType` / `hwaccel` / `get_format` / `av_hwframe_transfer_data`。4K HEVC/AV1 全靠 CPU。
- **无零拷贝**：即便将来加了硬解，现在也没有"硬件帧 → SDL 纹理"的通道。
- 队列偏小：`VIDEO_QUEUE_MAX_FRAMES = 16`（`player.cpp:18`）在 4K 下约 0.5 秒缓冲，抗抖动余量小。
- **RPC 延迟受主循环约束**（`ARCHITECTURE.md` §2 已如实记录：60fps 下 p50 ≈ 16.6ms），加重画质处理会进一步挤压。

### 2.4 功能缺失

| 缺失 | 依据 | 备注 |
|---|---|---|
| 字幕（`.srt`/`.ass`） | 全仓无字幕解码；`set_window_subtitle` 只是改**窗口标题**（`player_plugin.h:383`，`app.cpp:543-549`） | 中文用户的核心缺口 |
| 音轨 / 字幕轨选择 | 只取 `av_find_best_stream` 的第一条（`player.cpp:189-190`） | 多语 MKV 无法切轨 |
| 音频输出设备选择 | 固定 `SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK`（`app.cpp:121`） | SDL3 已提供 `SDL_GetAudioPlaybackDevices`（`vendor/sdl3/include/SDL3/SDL_audio.h:547`），只是没接 |
| 播放列表 / 媒体库 / 记忆播放 | 无 | `02-auto-resume` 只做单文件续播 |
| 音轨音量均一化、A-B 循环、逐帧步进、视频旋转/裁边 | 无 | 常规播放器标配 |
| 网络流 | **默认拒绝**（`app.cpp:166`） | 安全策略正确，保留 |

### 2.5 交互与界面

| 问题 | 依据 |
|---|---|
| **完全没有鼠标事件**：不能点按暂停、不能拖动进度、不能滚轮调音量 | `app.cpp:218-240` 的 `pollEvents()` 只处理 `SDL_EVENT_QUIT / KEY_DOWN / KEY_UP / WINDOW_RESIZED`。**而 `PP_TOPIC_UI_MOUSE "ui.mouse"` 在 SDK 里已经声明、在 `eventbus.cpp:15` 已经登记为 BAIL 模式，内核却从不派发它** —— 契约挖了坑没填 |
| OSD 非 ASCII 全部变 `?` | `osd.cpp:44-66`（`sanitize()`）+ `SDL_RenderDebugText`；`player_plugin.h:274` 明文写着"内置 8x8 点阵字体，支持可打印 ASCII；非 ASCII 以 '?' 呈现" |
| OSD 默认压画面 | 几何见 `app.cpp:407-426`：16:9 窗口播 16:9 素材 → 画面矩形≈满窗 → `osd.video_rect` 给不出可避让的黑边 → 面板盖住画面（§1.4 已实测）。`O` 键是**兜底**而不是默认 |
| 无窗口控件（无边框/标题栏菜单/右键菜单/无缩略图预览） | 无 |
| 字体只有 8×8 点阵，观感粗糙 | `osd.cpp:64` |

---

## 3. 设计原则：内核给「能力」，功能**仍然全是插件**

你要求保持"一切皆插件"的 Cordis 架构。这条能成立，但需要把边界划清楚 —— **不是所有东西都能做成插件**，硬塞会破坏体验：

**必须在内核里的（因为它就是"能力通道"本身）**

1. **解码/呈现管线**：硬解、NV12/GPU 纹理、色彩矩阵、呈现时机 —— 这是所有画面相关能力的底座，插件够不着（插件不链接 FFmpeg/SDL3，`AGENTS.md`「架构」段）。
2. **事件源**：鼠标/滚轮/触控必须由内核从 SDL 取出并派发到总线（现在缺的就是这一步）。
3. **通用叠加层能力**：让插件能把"位图叠加层"或"文字"画到画面上，而不必自己碰 SDL。
4. **真字体排版能力**：`osd.text` 支持 UTF-8 + 字号/度量，插件才有中文可读的 UI。
5. **媒体能力**：多轨枚举与切轨、字幕包送进解码器、音频设备枚举。

**仍然做成插件的（功能策略与业务）**

| 功能 | 形态 | 理由 |
|---|---|---|
| 字幕（选轨、样式、字体、外部字幕加载、字幕搜索） | 原生插件 `subtitles` | 策略复杂、可替换；渲染由内核提供的叠加层能力承载 |
| 播放列表 / 媒体库 / 续播 | JS 插件（Cordis） | 纯业务逻辑，热重载友好 |
| 鼠标 UI（进度条拖拽、点按、滚轮、悬浮控件） | 原生插件 `ui_controls` + `ui.mouse` 事件 | 像 `osd_stats` 一样画在 OSD 上即可 |
| 音轨/字幕轨/设备选择面板 | 原生或 JS 插件 | 调用内核能力即可 |
| 画质增强（去色带、锐化、胶片颗粒、超分） | 原生插件（帧处理链）或 GPU 着色器插件 | 正是"插件"最该干的事 |
| 音频设备切换、声道映射 | JS 插件 | 调用能力 |

> **关键设计取舍（必须写进 ARCHITECTURE.md）**：现在的帧处理链（`PP_FrameView` / `PP_PIXFMT_RGBA8888`，`player_plugin.h:132-134,216-225`）是**就地改 CPU RGBA**，这是"插件零拷贝"的优点，也是像素级效果的基础。改成 NV12/GPU 帧后**不能直接砍掉它**，正确做法是：
> **内核默认走 GPU 路径；只有当至少注册了一个帧处理器时，才额外物化一份 CPU RGBA 交给链**（并把这次物化的耗时计入统计，让"开了效果就掉帧"可观测）。GPU 侧的增强则走新增的 **GPU 帧处理链**。这样既保住画质/性能，又保住插件生态。

---

## 4. 分期方案

### P0 —— 正确性与地基（不修完，后面所有"画质验证"都没有意义）

#### P0-1 修复自检渲染证据 + 加严断言 —— **已完成（2026-09-12，B1）**

**实际做法**（与初版设想的差异已注明）：

- **上传语义**：`pumpAndUpload()` 改为返回 `bool`（是否真的上传成功）并新增 `force` 参数；
  `force=true` 跳过"同一帧不重复上传"的去重（`src/core/app.h`、`src/core/app.cpp`）。
  同时把 `SDL_UpdateTexture` 的返回值纳入判断，失败不再被当成成功。
- **恢复不变量**：自检的 `decode_and_display` 与 `seek` 两个循环**不再改写成员
  `lastUploadedIndex_`**，改用局部 `lastSeenIdx`。那个成员的不变量是"只有
  `pumpAndUpload` 会写"，自检去写正是本次 bug 的根因。
  *（初版设想是直接 `lastUploadedIndex_ = -1`；最终选了更明确、也不依赖哨兵值的 `force`。）*
- **像素判据**：新增可移植纯函数 `pp::rgbaColorRatioInRect()`
  （`src/core/util.h` / `util.cpp`），`render_and_capture` 断言
  ①上传成功 ②抓屏成功 ③**画面矩形内"恰好等于清屏色"的像素占比 < 80%**。
  清屏色写成常量 `kClearR/kClearG/kClearB` 并被 `renderFrame()` 复用，避免两处漂开；
  读回缓冲先 `SDL_ConvertSurface` 成 RGBA32 再取样（不假定后端字节序）；
  `renderFrame()` 记录画面矩形 `videoRectX_/Y_/W_/H_` 与 `videoRectValid_` 供取证使用。
- **配套**：`plugin_commands` 改为断言每条命令返回码为 0，失败时给出可操作提示
  （指出输出目录不在插件白名单内、可用 `--write-dir` 或把 `--out` 放进 `dist/`）。
- **单测**：`tests/unit/test_capture.cpp`（8 项，可移植，Linux CI 一并跑）+
  注册进 `build.py` 的 `UNIT_TEST_SOURCES`。

**验收证据（全部实跑）**：

| 项 | 结果 |
|---|---|
| 像素判据（同一工具、同一矩形 1278x720@(1,0)） | **修复前 86.60%（会被判红）→ 修复后 0.39%** |
| `--selftest`（输出在 `dist/` 内，即 `selftest.bat` 口径） | **10/10 PASS**，`render_and_capture` 详情报出占比 0.00%–0.39% |
| 新断言能判失败 | 输出目录放到白名单外时 `plugin_commands` 判 **FAIL** 并打提示；`snapshot -> -1` 不再被放过 |
| 单元测试 | **83/83 通过**（原 75 + 新增 8） |
| 冒烟 | `build.py --smoke` 两项 PASS |
| 机械门禁 | `tools/verify.py` **18/18 通过** |
| Python 测试 | `Ran 108 tests ... OK` |
| 可视证据 | 修复后抓屏里能看到真实影片画面（此前只有清屏色 + OSD） |

> **未做（如实登记）**：① 未跑 `tools/verify.py --selfcheck` —— 本次没有改动门禁本身，
> 按 AGENTS.md 的门槛表不要求；像素断言"能判红"由上面两条（修复前 86.60% / 输出目录越界）
> 直接证明。② `docs/ARCHITECTURE.md` 的线程模型/渲染段还没有补"`lastUploadedIndex_`
> 不变量 + 抓屏取证判据"这两行说明，留到与 P0-3/P0-4 的渲染改动一起写，避免同一段改两遍。

#### P0-2 定位并修复"黑屏/定格" —— **已完成（2026-09-12，B2）**

**复现结论（与初版推断的差异，很重要）**

初版推断的方向是"`finished()` 把'缓冲区此刻为空'判成'播放结束'，于是主时钟被冻结"。
方向对了一半 —— `finished()` 的语义确实有问题，但**真正的触发条件不是时序竞争，
而是一个确定性的状态污染**：

> `Player::open()` **漏复位 `demuxFinished_` 与 `drainSent_`**。第一个媒体读到 EOF 时
> 它们被置真；第二次 `open()` 之后，`demuxLoop()` 一进循环就命中
> `if (demuxFinished_) { sleep(20ms); continue; }` —— **一个包都不读**，
> 而 `finished()` 因为"缓冲区刚清空"当场为真 → `masterClock()` 返回冻结的 `extPts_`(=0)
> → `pumpFrame()` 永远取不出帧 → `tex_` 从不创建 = **永久黑屏**，
> 并且主循环立刻发 `media.eof` / `player.completed`。

所以它是**100% 可复现的**（不是时序偶发），触发条件是"同一进程里换片"——
即控制通道 `player.open`、插件 `open_media` 这两条路径。App 层状态
（`lastUploadedIndex_` / `completedFired_`）当时是复位了的，漏的是 Player 层，
自检又只打开一次媒体，所以长期没被碰到。

**复现方法（已固化成可复跑的两路取证）**

1. 黑盒：起一个隐藏实例 → 认证控制通道 → 采样 `player.stats` → `player.open` 换片 → 继续采样。
   修复前换片后 `position/master/video_clock/audio_clock` **全部恒为 0.000、`vqueue=0`、`abuf=0`**，
   日志里「已打开媒体」紧跟着一行「播放结束」。
2. 白盒：`--selftest` 新增 `reopen_media` 步骤（二次打开同一媒体 + 解码 10 帧断言出画）。

**做了什么**

- `Player::open()` 补齐 `demuxFinished_` / `drainSent_` 复位；并新增
  `vDrained_` / `aDrained_` 两个**显式排空标志**一起复位
  （解码线程处理完 drain 空包且 `receive_frame` 返回 EOF 时置真；`flushAfterSeek()` 与
  `open()` 置假）。
- `finished()` 从"队列此刻为空"改为**四条件**：demux 读完 ∧ 视频解码器排空（无视频轨则跳过）
  ∧ 音频解码器排空（无音频轨则跳过）∧ 包队列/帧队列/音频环形缓冲皆空。
  这样"刚 open、还没喂上第一批数据"与"主线程恰好把帧取空"都不会再被误判成已结束。
- `--selftest` 新增 `reopen_media` 步骤（放在 `plugin_hot_reload` 之后、
  统计快照之后，避免覆盖报告里 `playback` 的含义）。
- **未做**（Step 3）：原计划的"首帧上屏前不推进主时钟 / `openAudio()` 推迟到首帧就绪"
  **没有做** —— 用控制通道扫了 9 个素材（852x480 → 794 MB，含纯视频 mp4 与纯音频 m4a）
  **都没有复现出首开黑屏**。没有复现证据就不改行为，避免为了改而改。
  `masterClock()` 的"未到时长不许冻结"也没加：`finished()` 修对之后，冻结本身就只在
  真的放完时发生，再加时长钳制反而会让"时长元数据偏大"的素材永远结束不了。

**验收证据（全部实跑）**

| 项 | 结果 |
|---|---|
| `reopen_media`（二次打开后解码 10 帧） | 修复前 **FAIL / 0 帧**（故意注释掉两行复位复现）→ 修复后 **PASS / 10 帧** |
| 控制通道换片时间线 | 修复前 `position=0.000 decoded=0 vqueue=0 abuf=0` 冻结 4.8 s → 修复后 `decoded=17 vqueue=15 abuf=572160` 正常推进 |
| 换片后日志 | 修复前 `已打开媒体` → `播放结束`（立刻）→ 修复后正常播放 |
| 播到结尾回归（seek 到 duration−2s 播完） | 仍收到 `media.eof` + `player.completed`，`position=104.874/104.875`，`finished()` 没变坏 |
| `--selftest` | **11/11 PASS**（连跑 3 次 4.49–4.60 s，稳定） |
| 短媒体（0.5 s） | `10/10 通过，跳过 1 项`（0.96 s） |
| 超大窗口边界 | `--size 8192x8192 --selftest` → 11/11 PASS，抓屏按像素上限跳过落盘、像素取证照常 |
| 门禁与测试 | `verify.py` 18/18；`build.py --unit` 83/83；`--smoke` 两项 PASS；Python 108 OK |

#### P0-3 色彩管理（治"发灰"）—— **已完成（2026-09-12，B3）**

**先纠正规划里的一个错判。** 初版把"发灰"归因于**电平范围没处理**。实测推翻了它：

| 素材（YUV 值已知） | 期望 | 修复前实测 | 结论 |
|---|---|---|---|
| `yuv420p` 有限范围 Y=16 / 235，未标注 | (0,0,0) / (255,255,255) | **(0,0,0) / (255,255,255)** | **本来就对** |
| `yuvj420p` 全范围 Y=0 / 255，标 `pc` | (0,0,0) / (255,255,255) | **(0,0,0) / (255,255,255)** | **本来就对** |

swscale 对 `yuv420p`(有限) 与 `yuvj420p`(全范围) 的默认处理是对的，所以"范围"这条不需要动。
**真正的缺陷是：素材标注的色彩矩阵被完全忽略** —— 内核从不读 `AVFrame.colorspace`，
而 `sws_getContext()` 不接受色彩信息，于是 swscale 只能按分辨率猜（SD → BT.601）。

**做了什么**

- 新增可移植纯函数 `chooseYuvColor(avColorSpace, avColorRange, w, h)`（`util.h/.cpp`）：
  帧标注优先；未标注时用 **FFmpeg `vf_scale` 的 auto 规则**（> 576 行 BT.709，否则 BT.601）；
  范围未标注取有限范围。返回值即 `SWS_CS_*`，可直接喂 `sws_getCoefficients()`。
- `player.cpp` 建好 swscale 上下文后调 `sws_setColorspaceDetails()` 喂入
  `inv_table` + `srcRange`（**两者必须同时给**，只给 range 在级联路径上无效 —— 这条坑
  与规划里的提示一致，实测需要）。矩阵与范围一并纳入上下文重建判据。
- 取值优先级链补了第 ② 层：**帧 → 容器（`codecpar->color_space`）**。查 FFmpeg 源码确认
  h264 解码器会用 SPS VUI 覆盖 `avctx` —— `colorspace` 仅当
  `colour_description_present_flag` 存在时覆盖，而 `color_range` 只要
  `video_signal_type_present_flag` 存在就覆盖（`libavcodec/h264_slice.c:1119`）。
- **没有**改用 FFmpeg 9 推荐的帧式 API（`sws_scale_frame`，从帧取色彩属性、
  `sws_init_context` 已 deprecated）：那条路要连帧缓冲所有权一起改，留给 P0-4 的
  NV12/GPU 化重写一并做，避免同一段代码改两遍。已写进 `docs/ARCHITECTURE.md` §3b。

**验收证据（全部实跑）**

| 项 | 结果 |
|---|---|
| `tests/color_check.py`（新增的永久回归层） | **5/5 通过**；含"纯元数据 A/B" |
| 纯元数据 A/B（码流逐字节相同，只差容器标注） | 修复后输出差 **20** 级；修复前差 **0**（= 标注被完全忽略） |
| 标 BT.709 的 SD 素材 | 修复前 (201,8,30)（= BT.601 结果）→ 修复后 (217,28,28)（≈ 期望 219,30,30） |
| 这项检查能判红 | 去掉 `sws_setColorspaceDetails` 后 → **3/5，2 项 FAIL**（A/B 差 0） |
| 单元测试 | `tests/unit/test_color.cpp` 6 项 + 原有 = **89/89** |
| CI 接入 | `tools/ci.py` 新增 `color` 阶段（前提：Windows + vendor/ + dist/） |

**未做（如实登记）**：`color_primaries` / `color_trc` 内核完全不读 → HDR / 广色域素材会被
按 SDR 处理（过曝或发灰）。那属于 P2-3 的色调映射范围，且需要先有 GPU 管线；已写进
README「已知限制」。

#### P0-4 硬件解码 + NV12/零拷贝 + 上传路径 —— **部分完成（2026-09-12，B4）**

拆成三小步做，**做一步测一步**；每一步都以实测数据决定去留。

**P0-4a 接入 D3D11VA 硬解 —— 已完成并保留** ✅

- `openVideoCodec()`：`av_hwdevice_ctx_create(D3D11VA)` → `hw_device_ctx` + `get_format`
  回调；解码帧为 `AV_PIX_FMT_D3D11` 时 `av_hwframe_transfer_data()` 下载回内存。
  任何一步失败都自动退回软解（只打 INFO）。`PP_NO_HWDEC=1` 可强制软解（A/B 用）。
- `pickHwFormat()` 的**回退必须是软件格式**：候选列表可能全是硬件格式，
  照抄 `fmt[0]` 会让解码器拿到输出不了的格式。已按 `AV_PIX_FMT_FLAG_HWACCEL` 过滤。
- 状态可观测：日志 `硬件解码已生效: d3d11va`；`player.info` / `player.stats` 加 `hwdec`
  字段（**没有动 ABI** —— 走的是 control.cpp 自己拼的 JSON）。
- **实测（A/B，同一二进制切 `PP_NO_HWDEC`）**：见 README 验证结果 §16。
  摘要：**丢帧显著减少**（1080p60 22→6、4K 300 帧 37→11）；**CPU 只在软解最贵的
  场景才降**（4K HEVC 10bit 19.2→12.8 s，−33%），H.264 反而升高（下载+拷贝的代价）。
  **"硬解一定省 CPU"是错的，必须按素材分别看。**

**P0-4b NV12 纹理 + GPU 做 YUV→RGB —— 前提修好后重新启用，实测决定性收益（2026-09-12）** ✅

第一次尝试是净损失，回退过一轮（`displayed` 300→243、`dropped` 11→78）；
原因就是上面 P0-4b' 那条"启动即注册帧处理器"，不是 SDL 的问题。

- 做法：解码输出改 NV12、纹理改 `SDL_PIXELFORMAT_NV12`、按需把 NV12 物化成 RGBA 给插件；
  NV12 纹理的颜色空间只能建纹理时给（`SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER`）。
- **实测（同一份二进制用 `PP_NO_NV12=1` 切回 RGBA 做 A/B；绝对数随机器状态漂移，
  比值稳定，见下）**：

| 素材（8 s 窗口 ×2 轮） | RGBA | NV12 | CPU/帧 |
|---|---|---|---|
| 2160p30 H.264 | 103.2/98.8 ms，显示 98/95 帧、丢 85/78 | **29.1/25.0 ms，显示 182/212 帧、丢 0** | **−72.9%** |
| 1080p60 | 21.9/24.4 ms，丢 67/94 | **7.8/7.2 ms，丢 0~1** | −66.5% |
| 720p30 | 13.7/13.4 ms | **4.9/5.4 ms** | −62.7% |
| 2160p HEVC 10bit | 123.9 ms | **58.5 ms** | −52.8% |

- **关键结论：RGBA 主线程每帧 ~100 ms（跟不上 33 ms 预算），显示帧数只有 NV12 的一半且
  丢帧两位数；NV12 贴着解码速率走、丢帧为 0。**这才是用户能感知的收益，
  单看 CPU 百分比会低估它。
- **复查时抓到自己一个 bug**（值得记）：早期实现把 colorspace 的赋值放在"建纹理"分支里，
  判据里读到的恒为初值 0，于是 NV12 纹理**每帧都被销毁重建**（4K 每帧 12 MB 反复分配）。
  这个 bug 只惩罚 NV12 路径 —— 也就是说最初的 A/B 是"把 NV12 的账算坏后再比"。
  教训：**判据读的变量必须与赋值同序**，diff 复查不能只扫"改了什么"，要看"新条件会不会恒真"。
- 顺手得到两条**可复用的事实**（已实测，记在 README §16 与 `util.h` 注释里）：
  ① SDL 3.4.16 的 NV12 纹理可用，且 `SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER`
  确实生效（同一 YUV 按 601/709 建纹理 → 读出 `(202,10,32)`/`(219,30,30)`，
  与 swscale 逐级一致）；② 3.4.16 **没有** `SDL_SetTextureColorSpace`，色彩空间只能在
  建纹理时给；NV12 纹理的默认 colorspace 是 `SDL_COLORSPACE_JPEG`（全范围 BT.601），
  不显式设置就是错色。SDLK 的取值表已抄进 `util.h` 的注释备查。
- 新增 `PP_NO_NV12=1` 开关（强制 RGBA 路径）。**它的存在意义就是"同一份二进制做 A/B"** ——
  否则只能拿两次不同构建的数字比，版本一变就没法复核。

**P0-4b' 让插件按需占帧链 —— 已完成（2026-09-12）** ✅

`video_effects` / `snapshot` 的帧处理器原先在 `on_start` 注册、常驻到卸载。
现在改成"启用才注册、停用即撤销"（`syncChain` / `syncStamp`），默认状态下**链是空的**：
自检报告 `registry.frame_processors = 0`，`plugin_hot_reload` 显示 `帧处理器 0->0`，
`effect.status` 的 `procId=0`。这是 P0-4b 能生效的**前置条件**。

- 实现坑（值得记）：用编辑器"全部替换"把 `updateSubtitle(h)` 改成 `applyState(h)` 时，
  把**新写的 `applyState` 函数体里那次调用**也换了 → 无限自我递归 → 自检挂死 180 s。
  定位手法：起隐藏实例 + 控制通道逐条下发命令看哪条不返回（8 秒超时即判挂），
  比读日志快得多（日志有缓冲，最后一两行会丢）。
- 回归：自检 11/11、色彩 5/5、单测 89/89、门禁 18/18。

**P0-4d 自检的两个自伤型测量缺陷 —— 已修（2026-09-12）** ✅

这一条本来是"顺手修的"，结果它才是 2160p 判 FAIL 的真因。

1. **写证据 PNG 把自己搞丢帧**：写一张 720p PNG 实测 ~137 ms，期间主循环停摆而主时钟照走，
   回来时三四帧都已到点、被 `pumpFrame()` 整批弹掉 —— **每写一张固定丢 3 帧**。
   修法：证据帧先留在内存（帧对象引用计数，留 `shared_ptr` 即安全），测量循环结束后再落盘。
2. **无条件物化 RGBA**：每帧都调 `makeFrameView()`（NV12→RGBA 的 `sws_scale`），
   2160p 上每帧几十毫秒，于是量的是"物化开销"而非播放管线（当时被判 284/299 FAIL）。
   修法：只有"链上真有处理器"或"这一帧要存证据"时才物化。

结果：**2160p 从 FAIL(284/299) 变成 PASS(300/300)、丢帧 0；720p 丢帧 6→0。**
`decode_and_display` 的明细也补齐为**解码帧数 / 播放器丢帧 / 插件丢帧**三项并列
（以前只报插件丢帧，少帧时定位不了是哪一段丢的）。

**P0-4e 预热（preroll）：修掉"开头固定丢 6 帧" —— 已修（2026-09-12）** ✅

`Player::open()` 后进入预热态，主时钟冻结；第一帧上屏才解除，宿主同时补启动音频设备，
让 A/V 一起从 0 起跑。定位过程见 README §18（两次对照实验：无音轨素材否掉了"音频时钟粒度"
假设；3 s / 10 s 素材对照证明是固定启动成本而与时长无关）。

**未做**：~~seek 之后没有预热~~ —— **已补做（2026-09-12）**：`flushAfterSeek()` 里同样进入预热
（时钟冻在目标点），并通过 `Player::setSeekCallback` 让宿主**暂停音频设备**、第一帧上屏时
`stepAudioPreroll()` 一并恢复；与 `dropBefore_` 不冲突（预热只冻时钟，解码线程照常丢目标前的帧）。
实测：4K seek 到 5.0 s → seek 响应 position 恰为 5.000；seek 后 5 s `dropped +0`（修复前 24）、
显示 +115 帧；自检 `seek` 步骤仍 PASS。

**P0-4c 真零拷贝 —— 评估完成，判定"当前不可行"（2026-09-12，三组 PoC 实测）**

把 `ID3D11Texture2D` 直接包成 SDL 纹理（`SDL_PROP_TEXTURE_CREATE_D3D11_TEXTURE_POINTER`），
两条路线都实跑了：

| 路线 | 做法 | 实测结果 |
|---|---|---|
| 共用设备 | 拿 SDL 渲染器的 `ID3D11Device` 换进 FFmpeg 的 hwdevice | 解码器 execute **E_INVALIDARG**（SDL 建设备不带 `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`；`QueryInterface(ID3D11VideoDevice)` 虽返回 OK，执行时仍拒绝） |
| 跨设备包装 | FFmpeg 自己 device 解码（surface 带 `BIND_SHADER_RESOURCE`），SDL 直接包装渲染 | 包装"成功"、首帧渲染有内容，但随后**D3D11 Device Removed**（驱动移除设备、进程崩溃）—— 跨设备建 SRV 违反 D3D11 规则，驱动级拒绝 |

过程中确认的**可复用事实**：
- FFmpeg 9.0.1 支持"surface 带 SR 绑定"：在 `AVD3D11VAFramesContext.BindFlags` 或
  `AVD3D11VADeviceContext.BindFlags`（全局）上设 `D3D11_BIND_DECODER|D3D11_BIND_SHADER_RESOURCE`
  即可，FFmpeg 自己分配池（**不要**走"手动预创建 texture"路线——只给 texture 不给 pool 会崩，
  gdb 栈在 `av_hwframe_ctx_init` 内）。
- 替换 hwdevice 的 device 时必须**同时替换 device_context**（`GetImmediateContext`），
  只换 device 不换 context 会崩。
- SDL3 没有 `SDL_PROP_RENDERER_CREATE_D3D11_DEVICE_POINTER`（外部设备建渲染器入口不存在）。

**可行替代（未来）**：① 渲染层整体换 SDL_gpu API（`SDL_CreateGPUDeviceWithProperties`
支持外部 D3D11 设备）；② 等 SDL 增加外部 D3D11 device 入口；③ 原生 D3D11 渲染器（脱离 SDL
渲染器，改动面最大）。三者的共同前提是"解码与渲染共用一个带 VIDEO_SUPPORT 的设备"。
当前 NV12 半零拷贝路径已让 4K30 贴解码速率、丢帧 0（§P0-4b），真零拷贝的边际收益有限，
**判定：不做，记录在案**。

**验收口径的修正（重要，2026-09-12 二次修正）**

初版写的是"4K HEVC 上 `decoded≈displayed`、`dropped` 下降"。
第一次实测后改成"`dropped` 确实降了，但 4K 自检整体仍 FAIL —— 瓶颈已转移到
主线程每帧工作量（4K RGBA 纹理上传 = 33 MB/帧）"。

**这个二次结论有一半是错的**，必须记下来：那次 FAIL 里混着**自检本身的测量污染**
（每帧无条件物化 RGBA + 写证据 PNG 阻塞主循环，见 P0-4d），
真因是 NV12 路线没开。把两处测量缺陷修掉、NV12 启用后，**4K 自检是 PASS(300/300)、丢帧 0**。

正确的验收口径应该拆成三条，且**每条都要能单独证伪**：

| 口径 | 负责的改动 | 怎么量 |
|---|---|---|
| ① `dropped` 是否下降 | 硬解 | `--selftest` 明细里的"播放器丢帧" / 真实播放的 `dropped` 增量 |
| ② 主线程每帧是否跟得上 | 上传路径（NV12 / 零拷贝） | **`cpu/帧`**（同一二进制 A/B，别用 `--selftest`） |
| ③ 自检的 FAIL 是不是自检自己造成的 | 测量路径 | 看丢帧是否与"固定动作次数"成正比（§P0-4d） |

**把三件事混在一个数字里，会得出错误结论 —— 本轮真的错了两次。**

### P1 —— 能力扩展 + 插件化功能（**批次规划 v2，2026-09-13 定稿，待确认后动手**）

> 说明：下面是**规划**，尚未改任何实现代码。所有现状结论都带 `文件:行` 可复核，
> 所有外部依赖都做了**联网调研 + 本机实测**（实测输出附在各处）。

#### P1-0 结论卡（TL;DR）

| # | 结论 | 依据 |
|---|---|---|
| 1 | P1 拆成 **4 批**：a 字体 OSD → b 鼠标+叠加层 → c 字幕 → d 多轨/设备。a 是其余各项的前置 | c 需要 b 的叠加层；c 的字幕渲染依赖 a 的真字体 |
| 2 | **真字体用 `stb_truetype` v1.26**（195 KB，public domain / Unlicense 二选一），不引入 SDL_ttf / FreeType | 实测 raw 可达、体积；与项目"零依赖单头文件"倾向一致 |
| 3 | **微软雅黑不能随包分发**（方正版权，商业使用需授权）—— 随包字体只能用 OFL/Apache 的开源中文字体 | 多个来源一致；本机 `C:\Windows\Fonts\msyh.ttc` 不可分发 |
| 4 | 中文字体建议 **先走"运行期加载系统字体"**（零分发风险），随包 OFL 子集字体作为可选增强 | 合规风险最低且能立刻解决"中文不可读" |
| 5 | **libass 依赖树过重**（freetype + fontconfig + fribidi + harfbuzz），字幕先走 FFmpeg 自带文本字幕解码的轻路线 | 调研结论；轻路线零新增依赖 |
| 6 | 新增能力**全部走插件**：内核只提供"能力通道"（字体渲染、鼠标事件、叠加层、轨道枚举），功能仍由插件承载 | 架构红线：Cordis "一切皆插件" 不破 |

**建议顺序与收益**：P1-a 单独一批（收益最直接：中文界面立刻可读，且是 P1-c 的前置）；
P1-b + P1-c 合并为一批（叠加层是字幕的底座，一起做避免接口改两遍）；P1-d 最后或按需。

---

#### P1-1 现状（代码实证，逐条可复核）

| 项 | 现状 | 证据 |
|---|---|---|
| OSD 文字 | 用 SDL 调试点阵字 `SDL_RenderDebugText`，`sanitize()` 把 **非 ASCII 全部降级成 `?`** | `osd.cpp:44-55`（sanitize）、`osd.cpp:57-66`（text） |
| 文字宽度 | `strlen(s) * 8 * scale` —— 对 UTF-8 中文必然错（一个汉字 3 字节会被当成 3 个英文字符） | `osd.cpp:68-71`；CJK 全宽也完全没考虑 |
| 鼠标事件 | 主题 `PP_TOPIC_UI_MOUSE` **已注册为 BAIL**，但 `pollEvents()` **只处理键盘与窗口 resize**，没有任何鼠标分支 | `player_plugin.h:168`、`eventbus.cpp:15`、`app.cpp:272-276` |
| 鼠标负载结构 | `PP_MouseEvent` **不存在**（SDK 里没有定义） | 全仓搜索 `PP_MouseEvent` 无匹配 |
| 字幕 | 全仓无字幕解码；`set_window_subtitle` **只改窗口标题** | `player_plugin.h:383` |
| 叠加层 | 无通用叠加层；OSD 只能画文字/面板/矩形/线/频谱，插件无法推自定义位图 | `osd.h:22-32` |
| 多轨 | 只取"最佳"音视频流，`PP_MediaInfo` 无轨道数组 | `player.cpp` 的 `av_find_best_stream` |

---

#### P1-2 外部依赖调研（联网 + 本机实测）

**① `stb_truetype`（字体光栅化）**

| 项 | 值 |
|---|---|
| 版本 / 体积 | **v1.26，199,192 字节（≈195 KB）**（实测 `Content-Length`） |
| 许可 | 文件头原文：`public domain`；另提供 **MIT** 作为备选 —— **二选一，任选其一即可** |
| 可达性 | `https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h` → **200**（本机实测） |
| 关键能力 | `stbtt_InitFont`（从内存解析）、`stbtt_GetFontOffsetForIndex`（**支持 TTC 字体集合**，本机 `msyh.ttc` 正是 TTC）、`stbtt_GetCodepointHMetrics`、`stbtt_GetPackedQuad` |

**② 中文字体（合规关键）**

| 字体 | 许可 | 是否可随包分发 | 体积（实测） |
|---|---|---|---|
| 微软雅黑 `msyh.ttc` | **方正版权**（微软委托设计） | **否** —— 个人屏幕显示免费，商业使用/再分发需授权 | 本机已有（`C:\Windows\Fonts`） |
| 思源黑体 / Noto Sans CJK | **SIL OFL 1.1** | **是**（可商用、可修改、可与软件捆绑再分发；禁止单独出售字体文件） | 8,429,224 B（SourceHanSansCN 子集 OTF） |
| Noto Sans SC | SIL OFL 1.1 | 是 | 8,331,336 B（SubsetOTF/SC） |
| 文泉驿微米黑 | GPLv3 + Apache 2.0 | 是（GPL 与本项目一致） | — |

- **注意**：部分中文资料把思源黑体写成"Apache 2.0"，与官方 `LICENSE.txt`（OFL 1.1）不一致。
  **实施第一步必须是拉取 LICENSE 原文核对**，再写进 `THIRD_PARTY_NOTICES.md` —— 不能照抄二手资料。
- 本机**没有** Noto Sans CJK / 思源黑体（实测 `C:\Windows\Fonts` 只有 msyh/simhei/simsun/Deng 等系统字体）。

**③ 字幕渲染（libass）**

| 项 | 值 |
|---|---|
| 许可 | 仓库标注 **ISC**（与本项目 GPL-3.0 兼容）；FSF 目录记录为 **GPLv2+ / ISC / other-1** 混合 |
| 依赖树（重） | freetype + fontconfig + fribidi + harfbuzz（4 个原生库要一起构建并入库） |
| 结论 | 与本项目"零依赖"倾向冲突、构建工作量是量级跳跃。**建议 P1-c 先走轻路线**（FFmpeg 自带文本字幕解码），libass 作为后续可选项 |

---

#### P1-3 门禁约束（决定方案可行性，先认清再动手）

| 门禁 | 影响 | 说明 |
|---|---|---|
| `portable-modules` | **有利** | 只检查 `util/config/eventbus/registry` 八个文件（`tools/verify.py` 的 PORTABLE 清单）—— **`osd.cpp` 不在其中**，字体加载可以用 Windows API（如遍历 `C:\Windows\Fonts`）；但**纯逻辑（UTF-8 解码、宽度计算）要放 `util.cpp` 才能进 Linux 单测** |
| `release-payload` | **要同步** | 当前声明 **17 文件 / 127 MB**，门禁会拿 README 写的数字与 `dist/` 实际比对。随包字体会变成 18 文件 / ~135 MB → README 与 `dist/` 两边都要更新 |
| `third-party-notices` | **要登记** | 生成器有"许可事实表"，新增 stb（PD/Unlicense）与字体（OFL 1.1）两条，每条要有 `evidence`（原文依据）+ 核实命令 |
| `versions-align` | 要同步 | 若新增/升级依赖版本，README 依赖表要跟上 |
| **ABI** | **会变更** | `PP_MouseEvent`、`PP_OverlayApi`、轨道数组都是 ABI 新增 → `PP_ABI_VERSION` 2 → **3**，宿主与 5 个插件全部重编（`build.py` 一起编，天然满足） |
| `plugin-command-count` / `rpc-methods-align` | 要同步 | P1-d 新增 RPC 方法时，README / `tests/cli_check.py` / `docs/CLI.md` / `ARCHITECTURE.md` 四处声明要一致 |

---

#### P1-a 真字体 OSD（中文可读） —— 建议第 1 批

**目标**：`Osd::text` 支持任意 UTF-8（含中文），宽度计算正确；插件 `PP_OsdApi.text` 的
`utf8` 语义**不变**（本来就叫 utf8），只是不再降级成 `?`。

**设计**

1. `vendor/stb/stb_truetype.h`（单头文件；`vendor/` 已在门禁 `EXCLUDE_DIRS` 中，不进扫描）。
2. 新增 `src/core/font.cpp/.h`（或并入 `osd.cpp`）：字形图集（atlas）+ 码点→UV 缓存；
   绘制时逐码点 `SDL_RenderTexture`。
3. **纯逻辑放 `util.cpp`**：UTF-8 解码（`utf8Decode`）、东亚全宽判定（`isWideCjk`）、
   文本宽度计算 —— 这三项**可移植且有单测**（`tests/unit/test_utf8.cpp`）。
4. 字体来源（**三选一，需要拍板**）：

| 方案 | 做法 | 优点 | 风险 / 代价 |
|---|---|---|---|
| **A（推荐先做）** | 运行期加载系统字体，按优先级 `msyh.ttc → simhei.ttf → Deng.ttf → arial.ttf`，可用 `--font <路径>` / 配置覆盖 | **零分发风险**（不分发字体文件）；立刻解决中文可读 | 依赖目标机有中文字体；不同机器字形不同 |
| B（增强） | A + 随包一个 **OFL 子集字体**（用 fonttools 裁剪思源黑体，保留 ASCII + GB2312 常用字，目标 1~2 MB） | 表现一致、可控 | 需构建期裁剪工具；`release-payload` 数字变化；OFL 要求改名字体须改名并同许可 |
| C | A + 随包完整 OFL 字体（~8 MB） | 最简单 | 发布物 +8 MB（127→~135 MB） |

5. 回退：字体加载失败时**保留**现有 `SDL_RenderDebugText` + `sanitize()` 路径 —— 不能让
   "字体缺失"变成"界面全空"。

**涉及文件**：`vendor/stb/stb_truetype.h`（新）、`src/core/font.*`（新或并入 osd）、
`src/core/util.*`（UTF-8 纯函数）、`src/core/osd.cpp`、`build.py`（无新增编译依赖，头文件即可）、
`THIRD_PARTY_NOTICES.md` + `tools/gen_third_party_notices.py`（登记 stb，字体若随包再登记字体）、
`README.md`（验证结果新增一节）。

**验收（可机械判定）**

- [ ] `tests/unit/test_utf8.cpp`：UTF-8 解码（1~4 字节、非法序列）、CJK 全宽判定、
      `textWidth("播放") == 2 * textWidth("ab")` 级别的不变式 —— **≥8 项**，且可在 Linux 跑。
- [ ] 自检新增/改造步骤：抓屏里能读到中文路径与中文提示。
      **判据必须是像素级的**（沿用 P0-1 的教训：不能只看"文件生成了"）——
      做法：画一段中文与画等长 `?` 串的位图**必须不同**（哈希不同），且非背景像素占比 > 阈值。
- [ ] 无中文字体时不崩、不空屏（回退路径有单测/自检覆盖）。
- [ ] 回归：自检 11/11、色彩 5/5、门禁 18/18、单测总数同步到 README。

---

#### P1-b 鼠标事件通道 + 通用叠加层 —— 建议第 2 批（与 P1-c 同批）

**目标**：插件能收鼠标事件（并能"吞掉"），能推自己的位图叠加层（字幕/弹幕/水印/预览的通用底座）。

**设计**

1. `pollEvents()`（`app.cpp:265+`）增加分支：
   `SDL_EVENT_MOUSE_MOTION / _BUTTON_DOWN / _BUTTON_UP / _WHEEL`（类型见
   `vendor/sdl3/include/SDL3/SDL_events.h:183-186`），构造 `PP_MouseEvent`
   （type / x / y / 相对位移 / 按键 / 滚轮 / 修饰键 / 时间戳），
   `dispatch(PP_TOPIC_UI_MOUSE, &ev, sizeof(ev))` —— **BAIL 模式已登记**（`eventbus.cpp:15`）。
2. 新增 `PP_OverlayApi`：插件注册一个 RGBA 图层（数据指针 + 宽高 + 位置 + 时间区间 + alpha），
   内核在 `renderFrame()` 的 OSD 之后合成；**按 owner 自动撤销**（复用 `registry` 既有机制）。
3. ABI：`PP_ABI_VERSION` 2 → **3**（`PP_MouseEvent` 新增、`PP_OverlayApi` 追加到 `PP_HostApi` 末尾）。

**涉及文件**：`src/sdk/player_plugin.h`（ABI）、`src/core/app.cpp`（事件 + 合成）、
`src/core/osd.*`（叠加层管理）、`src/core/host.cpp`（API 表挂接）、`docs/ARCHITECTURE.md`、
`README.md`（ABI 版本号）。

**验收（已机械判定）**

- [x] 鼠标 BAIL 语义：事件总线单测 `tests/unit/test_eventbus.cpp::mouse_bail_consumed_stops_chain`
      （3 个订阅者，a.consumed=1 后 b/c 不再收到，dispatch 返回 1）；内核 `app.cpp::handleMouse`
      派发 `PP_TOPIC_UI_MOUSE`，插件吞掉后"点画面跳转"不再触发。合成事件，不依赖真实硬件。
- [x] 叠加层按 owner 撤销无残留：自检步骤 `overlay_owner_revoke` 像素级断言 —— 推红方块图层
      （owner=1）渲染后数到 1600 红像素、`overlayClear(owner)`（即 `Host::revokeAll` 路径）
      后重渲染红像素归 0、图层数归 0；对照 FillRect 块始终 1600，排除"纹理渲染失效"误判。
      关键修正：D3D11 后端 `SDL_RenderReadPixels` 读回是 BGRA，必须先 `SDL_ConvertSurface(...,
      SDL_PIXELFORMAT_RGBA32)` 再取样（与 `render_and_capture` 同一模式），否则红像素判据恒假阴性。
- [x] ABI 版本号与插件清单一致：`PP_ABI_VERSION=3`、`ARCHITECTURE.md` 鉴权示例 `abi:3`、
      `THIRD_PARTY_NOTICES.md` 同步；`plugin-manifest`（5 DLL 哈希）、`versions-align` 门禁均 PASS；
      全量 `build.py` 重编宿主 + 全部 5 个插件天然一致。

---

#### P1-c 字幕（做成插件） —— 建议第 2 批尾 / 第 3 批

**目标**：能加载并显示外部字幕与内挂文本字幕轨。

**设计（先轻路线）**

1. 内核提供**能力通道**：字幕轨枚举 / 切换、把字幕包交给 FFmpeg 解码器
   （`AV_CODEC_ID_SUBRIP` / `AV_CODEC_ID_ASS` 等），把结果（文本 + 起止时间）以事件或
   `PP_SubtitleApi` 暴露 —— 内核**不做渲染**。
2. `subtitles` 原生插件：加载外部 `.srt/.ass`、解析时间轴、剥离 ASS 样式标签
   （轻路线只保留文本与换行）、用 **P1-b 的叠加层**或 **P1-a 的字体**渲染。
3. **重路线（后续可选项）**：接入 libass 以获得完整 ASS 特效 ——
   代价是引入 freetype/fontconfig/fribidi/harfbuzz 四件套，**建议单独立项**，不在本批。

**验收（可机械判定）**

- [ ] `--selftest` 新增 `subtitle_render` 步骤：
      **字幕关闭时该步骤 SKIP 且不计入通过数**（沿用既有口径：跳过 ≠ 通过）。
- [ ] 像素级断言：抓屏里**能读出字幕文字**（依赖 P1-a 真字体 + P0-1 抓屏判据）。
- [ ] 时间轴正确性：给定 `.srt`，在指定时刻断言"该条字幕在/不在"（可用控制通道 seek 到时刻采样）。

---

#### P1-d 多轨与音频设备选择 —— 建议最后（或按需）

**目标**：多音轨/字幕轨切换、音频输出设备选择。

**设计**：内核枚举 `AVMEDIA_TYPE_AUDIO/SUBTITLE` 全部轨道；`SDL_GetAudioPlaybackDevices`
接出设备列表与切换；`PP_MediaInfo` 增加轨道数组（**ABI 变更，与 P1-b 一起做可省一次 ABI 升级**）。
插件：`track_selector` / `audio_device`。

**验收**：RPC 新增 `player.tracks` / `player.select_track` / `audio.devices` /
`audio.set_device`，同步 `ARCHITECTURE.md` §7.2 方法一览（`rpc-methods-align` 门禁会拦）；
自检加"切换音轨后解码仍正常"步骤。

---

#### P1-4 风险与不足（如实登记）

| 风险 | 说明 | 应对 |
|---|---|---|
| **字体合规** | 误用不可分发字体会造成侵权 | 方案 A（不分发）优先；若随包，**先拉 LICENSE 原文核对**再登记；不照抄二手资料的许可描述 |
| **字形图集内存/质量** | 中文字形多，全量烤进图集不现实 | 按需烤（LRU 缓存，上限如 2048 字形）；字号按整数档缓存 |
| **ABI 升级波及插件** | v2→v3 后旧插件加载失败 | `build.py` 同时编译宿主与全部 5 个插件，天然一致；`plugin-manifest` 门禁会拦哈希不符 |
| **叠加层性能** | 每帧上传插件 RGBA 图层可能拖慢 4K | 只在图层内容变化时重传（脏标记），并在 4K 上用 `cpu/帧` 复测（方法见 DEVELOPMENT §6b） |
| **字幕时间轴与 A/V 同步** | 字幕要跟主时钟而非解码顺序 | 明确以 `masterClock()` 为准；seek 后字幕要按新位置重算 |
| **libass 依赖树** | 4 个原生库要一起构建 | 本批不走这条路；若要上，单独立项评估 |

**本规划未覆盖（不在 P1 范围）**：HDR/色域（P2）、GPU 着色器画质链（P2）、播放列表 UI（P2）、
真零拷贝（P0-4c 已判定不可行）。

---

#### P1-5 需要你拍板的 4 个决策点

1. **字体方案**：A（系统字体，零分发风险）/ B（A + OFL 子集字体随包）/ C（A + 完整 OFL 字体 +8 MB）？
   —— 我建议 **先 A，验收后再决定要不要 B**。
2. **批次划分**：P1-a 单独一批，P1-b + P1-c 一批，P1-d 最后 —— 还是希望一次性做完 P1 全部？
3. **ABI**：P1-b 与 P1-d 都有 ABI 变更，**是否合并到同一次 ABI 升级（v3）**（一次性做完 b+d 的 ABI 部分）？
4. **libass**：P1-c 明确只做轻路线（FFmpeg 文本字幕），libass 留到以后单独立项 —— 同意？

### P2 —— 看得好 / 好用

| 项 | 内容 | 依据/参考 |
|---|---|---|
| P2-1 高质量缩放 | 用 **SDL_GPU 自定义 fragment shader** 实现 Lanczos/EWA 上采样与正确下采样（`SDL_gpu.h` 已随包；`SDL_SetDefaultTextureScaleMode` 已可用作过渡方案） | mpv 默认 `lanczos`、high-quality 档 `ewa_lanczossharp`；libplacebo 的缩放档位表 |
| P2-2 去色带 + 抖动 | GPU 后处理：deband（迭代/阈值/半径/颗粒）+ error-diffusion dither | mpv `deband=yes deband-iterations=4` / `dither=error-diffusion` |
| P2-3 HDR / 色调映射 | 静态元数据起步（BT.2390），再考虑直方图动态峰值 | D3D11/D3D12 后端本身已支持 HDR 输出与 tone mapping 参数 |
| P2-4 播放列表 / 媒体库 | JS 插件（Cordis）：队列、播放模式、拖拽载入、记忆播放扩展 | 现有 `02-auto-resume` 可复用 |
| P2-5 音频 | 倍速变调补偿（现在靠 `SDL_SetAudioStreamFrequencyRatio`，会变调）、音量均一化、声道映射 | mpv `audio-pitch-correction` |
| P2-6 其他 | 逐帧步进、A-B 循环、旋转/裁边/去交错、缩略图预览（用叠加层能力）、截图带元信息 | MPC-HC/PotPlayer 常规项 |

> **重度备选（需要单独评审）**：把渲染层整体换成 **libplacebo**（mpv `gpu-next` 的内核），一次拿到色彩管理 + 高质量缩放 + deband + HDR tone mapping。代价：新增重型依赖（依赖 Vulkan）、构建复杂度大、与 SDL 渲染器的边界要重新设计。**建议放到 P2 之后，等 P0/P1 落地、看清楚瓶颈再决定**，不要现在动。

---

## 5. 验收标准与验证命令汇总

| 项 | 验收标准（可执行） | 命令 |
|---|---|---|
| P0-1 | 抓屏直方图中背景色占比 < 15%；纹理禁用时该检查报红 | `build.py --smoke` → `pluginplayer selftest` + 新增 Python 单测读 BMP |
| P0-2 | 构造用例：修复前 FAIL / 修复后 PASS，并固化进 `tests/` | 新增 `tests/` 回归 + `--selftest` |
| P0-3 | limited/full 参考素材的黑白位落在预期窗口 | 新增 Python 单测（读 PNG 像素）+ 对比图 |
| P0-4 | 4K HEVC：`decoded≈displayed`、`dropped` 下降；CPU 占用下降 | `--selftest` + 任务管理器 + `objdump -p` |
| P1-1 | 抓屏可读出中文 | `--selftest` 抓屏 + Python 断言 |
| P1-2 | 鼠标 BAIL 语义与叠加层撤销无残留 | `build.py --apitest` 扩展 |
| P1-3 | `subtitle_render` 步骤通过；关字幕时记 SKIP 不计通过 | `--selftest` |
| P1-4 | 新 RPC 方法可调用 | `tests/cli_check.py` 扩展 + `tools/rpc_client.py` |
| 全部 | 门禁与单测不回归 | `tools/verify.py` → `tools/verify.py --selfcheck` → `build.py --unit` → `python -m unittest discover -s tests\python` → `ci.bat` |

---

## 6. 必须同步修改的「门禁与文档」清单（漏了 `verify.py` 就会红）

这个仓库的铁律是"两处必须相等的约束写成可执行检查"。因此每一项改动都带一串连带修改：

| 改动 | 连带必须同步 |
|---|---|
| 新增/改 `PP_HostApi` / `PP_OsdApi` / `PP_MediaInfo` 字段 | `PP_ABI_VERSION` **2 → 3**（`player_plugin.h:78`）；重编全部 5 个原生插件；同步 `docs/PLUGIN_GUIDE.md`、`examples/hello_plugin`、`docs/ARCHITECTURE.md` §8；新增 `versions-align` 对 ABI 版本的断言 |
| 新增插件 / 新增命令 | `plugin-command-count`（现 15）、`native-plugin-set`（现 5）、`js-plugin-count`（现 3）三处门禁的期望值；`README.md`「内置插件」表；`docs/CLI.md` |
| 新增 RPC 方法 | `rpc-methods-align`（现 23 个）+ `docs/ARCHITECTURE.md` §7.2 方法一览 |
| 新增 CLI 命令 | `cli-commands-align`（现 25 个）+ `README.md` + `docs/CLI.md` |
| 新增数值边界 | `arg-bounds-align`（现 13 项）+ `tools/pp_cli.py` |
| 新增/升级依赖（SDL3、libass、stb、字体） | `THIRD_PARTY_NOTICES.md`（**生成物，必须重跑 `tools/gen_third_party_notices.py`**）；`versions-align`；`DEPS_SHA256`；`fetch_deps.py` |
| 发布载荷变化（新增 DLL/字体） | `release-payload`（现 **17 个文件 / 127 MB**）+ `README.md` 同口径表述 |
| 新增文档 | `docs-index`（现 4 份）—— **本文件若移入 `docs/`，必须同时登记进 `docs/README.md`** |
| 新增测试 | `tests/python/test_verify.py` 里有断言要求"每条新门禁必须配套 `--selfcheck` 破坏用例" |
| 目录结构变化 | `readme-tree`（现 56 个条目） |

---

## 7. 风险与「必须实跑验证」清单（不许"看起来没问题"）

1. **SDL3 3.4.16 的 NV12 上传路径是否已含 2025-12-05 那批修复** —— 未验证。第一件事就是写一个最小程序验证 NV12 纹理上传+显示。
2. **随包 SDL3.dll 的着色器能力**（HDR/tone mapping 扩展、SDL_GPU 后端可用性）—— 需在目标机器实测，不能只看头文件。
3. **本机 grep 结论是"源码级"证据，不等于"运行时"证据**：例如"引擎永远走 D3D11"这条是从 SDL 源码 driver 优先级推出的（Windows 上 `D3D11_RenderDriver` 排第一），应在运行时用 `SDL_GetCurrentRenderDriver()` 打印确认，并在启动日志里固化这一行。**（这条本身就是个小改进项：现在日志没打印实际渲染后端。）**
4. ~~**`finished()` 冻结假设尚未复现**（§2.1-B）—— 必须复现后再修，否则可能改错地方。~~
   **已解决（B2）**：复现了，而且真因是确定性缺陷（`open()` 漏复位两个标志），
   不是竞争；同时 `finished()` 的语义也确实被修严了（四条件）。回归用例已固化。
5. **ABI v3 是一次全插件重编**，`plugin-manifest.sha256` 也会全变；`--release` 出来的口径（文件数/体积）会变，README 与门禁都要跟着改。
6. **libass 的依赖链**（freetype / harfbuzz / fribidi）在 MinGW 下的可用性与体积需评估；若不顺，退回"FFmpeg 自带文本字幕"的轻路线。
7. **中文字体许可**：随包字体必须许可清晰并登记，不能随手塞一个微软雅黑。
8. **操作提示（本次踩到）**：用 PowerShell 的 `Start-Process -ArgumentList` 调用本播放器会**破坏中文路径**（实测 `open_media` 直接 FAIL）。写自动化脚本请用调用运算符 `&` 或 `cmd /c`，否则会得到假失败。

---

## 8. 建议执行批次

| 批次 | 内容 | 门禁要求 |
|---|---|---|
| **B1** | P0-1（证据链修复 + 加严断言）→ 立刻给出"修复前/修复后"对比图 | `verify.py` + `verify.py --selfcheck` + 单测 |
| **B2** | ~~P0-2 复现 → 修复 → 固化回归用例~~ **已完成 2026-09-12** | `verify.py` 18/18 + `--selftest` 11/11 + 控制通道两路取证 |
| **B3** | ~~P0-3 色彩管理~~ **已完成 2026-09-12**（实测后收敛为"矩阵 + 容器标注"；范围本来就对） | `tests/color_check.py` 5/5 + 单测 89/89 + `verify.py` 18/18 |
| **B4** | ~~P0-4 硬解 + 零拷贝~~ **部分完成 2026-09-12**：4a 硬解已接入并保留；4b（NV12 纹理）实测净损失、已回退；4b'（插件按需占帧链）与 4c（真零拷贝）待做 | 自检 11/11 + 色彩 5/5 + A/B 基准 + `verify.py` 18/18 |
| **B5** | ABI v3 一次性落地（把 P1-1/P1-2/P1-4 需要的新能力一起加，避免多次重编全部插件） | `verify.py --selfcheck` + `--apitest` |
| **B6** | P1-1 真字体 OSD / P1-2 鼠标通道 / P1-4 多轨与设备 | 同上 |
| **B7** | P1-3 字幕插件（先轻路线打通，再上 libass） | 同上 + 字幕用例 |
| **B8** | P2 画质链（GPU 着色器缩放 / deband / dither）与播放列表等插件 | 同上 + 性能对比 |

**每批次结束都要**：`tools/verify.py` → `tools/verify.py --selfcheck` → `build.py --unit` → `python -m unittest discover -s tests\python` → `ci.bat`，并把"改了/为什么/怎么验证"写进 README 的验证记录；**未修项登记进「已知限制」并写明为什么不修**（`AGENTS.md`「汇报要求」）。

---

## 9. 一句话总结

> 这个仓库的**工程纪律是好的**（门禁、自检、ABI 规则都在），问题在于**它一直在用一张不含画面的"证据图"证明自己画得出来**，并且**画质链路只做到"能播"**。
> 所以顺序必须是：**先把眼睛修好（P0-1/P0-2），再把画面修对（P0-3/P0-4），最后才是好用（P1/P2）** —— 同时严守"内核给能力、功能做插件"这条线。
