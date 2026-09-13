# 架构说明

## 1. 分层

```
┌──────────────────────────────────────────────────────────────────────┐
│ 插件层                                                                │
│  ┌────────────────────────┐   ┌──────────────────────────────────┐   │
│  │ 原生插件 plugins/*.dll  │   │ JS 插件 host-js/plugins/*.mjs     │   │
│  │ 走 C ABI（同进程）      │   │ 走 JSON-RPC（跨进程）             │   │
│  └───────────┬────────────┘   └──────────────┬───────────────────┘   │
└──────────────┼───────────────────────────────┼───────────────────────┘
               │ PP_HostApi*                   │ TCP 127.0.0.1:7911
┌──────────────▼───────────────────────────────▼───────────────────────┐
│ 插件宿主 Host                                                         │
│   EventBus（emit/waterfall/serial/bail）                              │
│   ServiceRegistry（ctx.<key> 的对应物）                                │
│   CommandRegistry（可被 UI / RPC / 其它插件调用）                       │
│   FrameChain（帧处理链，waterfall 特化）                                │
│   Config（plugin.<槽位>.<键> 前缀隔离）                                 │
│   RpcServer（把同一套能力暴露给进程外）                                  │
└──────────────┬───────────────────────────────────────────────────────┘
               │
┌──────────────▼───────────────────────────────────────────────────────┐
│ 播放内核 Player                                                       │
│   demux 线程 → PacketQueue ─┬→ 视频解码线程 → sws_scale → FrameQueue   │
│                             └→ 音频解码线程 → swr_convert → AudioRing │
│   主线程：pumpFrame（按主时钟取帧）→ 帧处理链 → SDL_UpdateTexture      │
│          → RenderTexture → OSD 阶段 → Present                        │
│   音频：SDL_AudioStream 回调从 AudioRing 取数并推进音频时钟             │
└──────────────────────────────────────────────────────────────────────┘
```

## 2. 线程模型

| 线程 | 职责 | 注意 |
|---|---|---|
| 主线程 | SDL 事件循环、`pumpFrame`、帧处理链、渲染、OSD、事件分发 | **插件的全部回调都在这里**（见下） |
| demux 线程 | `av_read_frame` → 分派到视频/音频包队列；处理 seek | 通过 `interrupt_callback` 让 seek/abort 能打断阻塞读 |
| 视频解码线程 | 收包解码 → `sws_scale` 转 RGBA → 帧队列（满则等） | **不调用插件**；只把帧放进 FrameQueue |
| 音频解码线程 | 收包解码 → `swr_convert` 转 S16 → 环形缓冲 | **不调用插件**；只负责填环形缓冲 |
| SDL 音频线程 | 音频回调：从环形缓冲取数 → `SDL_PutAudioStreamData` | 回调里只做搬运，绝不做重活，也不碰 HostApi |
| RPC accept / 客户端线程 | 控制通道：收报文 → 入队 → 等主线程执行 → 回包 | 客户端线程**不直接调用宿主 API**（见下） |

**关键约束 1：`AVCodecContext` 非线程安全。**
`avcodec_flush_buffers` 必须由**拥有该上下文的那条解码线程**调用。
本项目通过「包序号（serial）」让解码线程自己发现 seek 并 flush：

```
flushAfterSeek()  清队列 / 清缓冲 / serial++      ← demux 线程
解码线程：取包 → 若 包serial != 上次处理的 serial → avcodec_flush_buffers()  ← 解码线程
        解出的帧若 包serial != 队列当前 serial → 丢弃（在途旧数据）
```

> 这条规则是踩出来的：早期把 `avcodec_flush_buffers` 放在 demux 线程，
> 与解码线程的 `avcodec_receive_frame` 并发访问同一个 context，
> 表现为 **-O2 下偶发段错误 + h264「Invalid NAL unit size」**。

**关键约束 2：插件回调只在主线程执行。**
`video.frame` / `audio.samples` 的**数据**由解码线程产出，但**回调**是主线程
取出数据后再调用的 —— 所以插件里不需要为自己的状态加锁，也绝不能阻塞。
（早期版本的 `player_plugin.h` 把这一点写反成"回调可能在解码线程"，已更正。）

**关键约束 3：控制通道请求在主线程串行执行。**
RPC 客户端线程只做「收报文 → 入队 → 等结果 → 回包」，真正的 `handleRequest`
由主循环每轮调 `RpcServer::pumpMainThread()` 在主线程抽干执行。
这样 `Player`/`Host` 的大量非原子状态（`extPts_`、`lastAudioClock_`、当前帧、
SDL 音频流…）只被一个线程触碰。
**代价**：RPC 单次延迟被主循环帧期约束（60fps 下实测 p50 ≈ 16.6ms，吞吐约 420 req/s）。
（早期版本让 RPC 线程直接调用宿主 API，JS 宿主每 5 秒轮询一次 stats 就会与主循环并发
改写这些字段 —— 是一处真实的数据竞争。）

## 2b. 硬件解码（D3D11VA）

只在 `openVideoCodec()` 里尝试，**任何一步失败都不报错，只打日志并自动退回软解** ——
"能硬解就硬解、不能就软解"必须是自带的，不能让用户去配。

```
av_hwdevice_ctx_create(D3D11VA) → vCtx_->hw_device_ctx = av_buffer_ref(...)
                                → vCtx_->get_format = Player::hwGetFormatCb
解码线程：receive_frame → 帧格式是 AV_PIX_FMT_D3D11 → av_hwframe_transfer_data(→ NV12)
```

要点：

- **`get_format` 必须给出可用的回退**。候选列表里可能全是硬件格式，若照抄 `fmt[0]`
  而当硬件不可用，解码器会拿到一个它输出不了的格式，之后每帧都失败。
  `pickHwFormat()` 在找不到目标硬件格式时**挑第一个软件格式**（校验
  `AV_PIX_FMT_FLAG_HWACCEL` 位）。
- **"半零拷贝"**：解码在 GPU，`av_hwframe_transfer_data()` 把帧下载回系统内存（NV12），
  之后的色彩转换/上传照旧。真正的零拷贝（把 `ID3D11Texture2D` 直接包成 SDL 纹理）
  要求 FFmpeg 与 SDL 共用同一个 D3D11 设备，改动面更大，**尚未做**。
- **可选关闭**：环境变量 `PP_NO_HWDEC=1`（与 `PP_WEB_REMOTE` / `PP_RPC_TOKEN` 同款惯例），
  用于排障与 A/B —— 硬解出问题时能一键确认"是不是硬解的锅"。
- 状态可观测：日志里 `硬件解码已生效: d3d11va（帧格式 d3d11 → 下载为 nv12）`，
  控制通道的 `player.info` / `player.stats` 里也有 `hwdec` 字段（空串 = 软解）。

**为什么这是"半"零拷贝、以及实测的性能账**（详见 README 验证结果 §16）：
下载回内存这一步在 4K 下要拷 ~12 MB/帧。实测硬解**显著减少丢帧**
（1080p60：22 → 6；4K 300 帧：37 → 11），但 **CPU 只在软解最贵的场景才降**
（4K HEVC 10bit：19.2 s → 12.8 s），H.264 上反而升高（6.0 s → 8.6 s）——
因为省下的解码时间抵不过下载+拷贝。**要让硬解普遍划算，必须走到零拷贝。**

## 3. A/V 同步

- **音频时钟**：`ringBasePts + consumedBytes / bytesPerSec`。
  `consumedBytes` 由音频回调累加，再减去 `SDL_GetAudioStreamQueued`（SDL 内部尚未播出的字节），
  使时钟贴近真实播放位置。
- **主时钟**：有音频且音频时钟在推进 → 用音频时钟；否则用墙钟。
- **停滞保护**：若音频时钟 350ms 内没有推进（典型场景：默认音频设备是**未连接的蓝牙耳机**，
  WASAPI 打开了设备但不出声），自动退回墙钟并同步基准，避免整机播放卡在 0 秒。
- **视频取帧**：`pumpFrame()` 每轮把 `pts <= master + 4ms` 的帧全部弹出，**只保留最后一帧**，
  中间的直接计为 dropped —— 落后时自动跳帧追进度。
- **预热（preroll）**：`open()` 之后到第一帧上屏之前，主时钟**冻结不动**，宿主也把音频设备
  的启动推迟到那一刻，让 A/V 一起从 0 起跑。不这样做的话，解码器冷启动的 ~0.2 s 里主时钟
  已经跑掉了，解出来的头几帧一进队列就"迟到"，被上面那条规则整批弹掉
  （实测固定丢 6 帧，与素材长短无关）。兜底：预热超 2 s 自动放开并记 WARN。
  **seek 后同样预热**：`flushAfterSeek()` 把时钟冻在目标点、宿主暂停音频设备
  （`setSeekCallback`），第一帧上屏后恢复 —— 修掉"seek 后整批丢帧"
  （4K 实测一次弹丢 24 帧 → 0）。与 `dropBefore_` 不冲突（预热只冻时钟，
  解码线程照常丢目标前的帧）。
- **暂停/变速**：暂停时墙钟冻结、音频回调直接返回（自然静音）；
  变速用 `SDL_SetAudioStreamFrequencyRatio(stream, speed)` 做重采样，视频看主时钟。

### 3a. 成批丢帧的诊断口径

`pumpFrame()` 一次弹掉多帧（`dropped > 0`）时会打一条 WARN，带四个数：

```
丢帧 3（主时钟 0.137，本帧 pts 0.133，距上次取帧 137.0 ms，队列深 13/16）
```

判读方式（这是把"掉帧"拆成两种成因的关键）：

| 现象 | 含义 |
|---|---|
| **距上次取帧** ≫ 循环周期（正常 2~3 ms） | 主线程被别的活儿占住了（写盘、建纹理、抓屏…） |
| 队列深度长期贴上限 | 解码供得上（积压），瓶颈在消费端 |
| 队列深度长期为 0 且主时钟领先 | 解码供不上（或被硬件解码节流），是真正的性能不足 |

`--selftest` 的 `decode_and_display` 明细里也并列三项 —— **解码帧数 / 播放器丢帧 /
插件丢帧**。只报"插件丢帧"曾经让一次误判拖了很久（少帧时看不出到底是哪一段丢的）。

## 3b. 色彩还原（矩阵与电平范围）

YUV→RGB 必须用**素材自己标注的那套矩阵**，不能靠猜。取值优先级链：

```
fr->colorspace / fr->color_range                 ① 逐帧标注（H.265/AV1 会逐帧给）
   ↓ 缺失时
stream->codecpar->color_space / color_range      ② 容器/流级标注（mp4 colr、mkv Colour）
   ↓ 仍缺失
按分辨率推测：> 576 行走 BT.709，否则 BT.601      ③ 与 FFmpeg vf_scale 的 auto 规则一致
```

- 选择逻辑是**可移植纯函数** `chooseYuvColor()`（`util.h`）。它的返回值直接就是
  libswscale 的 `SWS_CS_*` 取值，喂给 `sws_getCoefficients()` 即可。纯逻辑放在 util
  里才能在 Linux CI 上跑单测（`tests/unit/test_color.cpp`）。
- **为什么需要 ②**：h264 解码器会用 SPS VUI 覆盖 `avctx->colorspace` / `color_range`
  （`libavcodec/h264_slice.c:1119`：`colorspace` 仅在 `colour_description_present_flag`
  存在时覆盖，`color_range` 只要 `video_signal_type_present_flag` 存在就覆盖）。
  码流没声明时容器标注是唯一线索，而 `codecpar` 归解复用器所有、解码器覆盖不到。
- **为什么用 `sws_setColorspaceDetails()` 而不是帧式 API**：`sws_getContext()` 一次建好
  上下文且不接受色彩信息，只能在建好后补设 —— `inv_table` 与 `srcRange` **必须同时给**，
  只给 range 在级联路径上无效。FFmpeg 9 更推荐 `sws_scale_frame()`（直接取帧上的色彩
  属性，`sws_init_context` 已标 deprecated），但那条路要连帧缓冲所有权一起改，
  目前仍用 `sws_setColorspaceDetails()` —— **未做的迁移**，不要误以为已经用上新 API。
- 矩阵与范围都进 **sws 上下文的重建判据**：素材中途换标注（合流、拼接）时必须重建，
  否则会拿旧矩阵还原新画面。
- **未做**：色域（`color_primaries`）与传输函数（`color_trc`）不参与运算，HDR 素材会被
  当 SDR 处理。见 README「已知限制」。
- 回归：`tests/color_check.py`（现造参考素材 + 读像素比对期望 + 同码流不同标注的 A/B）、
  `tests/unit/test_color.cpp`（纯逻辑）。

## 4. 精确 seek

`AVSEEK_FLAG_BACKWARD` 只能回到**前一个关键帧**。对关键帧稀疏的文件（例如 x264 默认
keyint=250，8 秒视频只有 1 个关键帧），seek 到 4.8s 会实际落在 0s。

处理方式（`dropBefore_`）：seek 后把目标时刻之前的解码输出全部丢弃
（视频帧、音频帧都丢），直到时间戳追上目标；同时主时钟立即乐观置为目标值，
这样用户看到的是「立刻跳到 4.8s」而不是「从头快进」。

实测：目标 4.80s → `seek` 后位置 4.99s（见自检报告）。

## 4b. 播放结束判定与跨媒体状态（**事故高发区**）

`Player` 同时被三种线程读写，其中有一批**跨媒体状态**：它们描述"这一部片子的进度到哪了"，
必须在 `open()` 里逐个复位。历史上漏掉任何一个，表现都是"换片/二次打开后不正常"。

### 播放结束（`finished()`）要四个条件同时成立

```
demuxFinished_                        // demux 线程读到 EOF 并投了 drain 空包
∧ (无视频轨 ∨ vDrained_)               // 视频解码器已排空（收到 drain 包后再吐不出帧）
∧ (无音频轨 ∨ aDrained_)               // 音频解码器已排空
∧ vq_/aq_ 空 ∧ vframes_ 空 ∧ 音频环形缓冲空
```

**为什么不能只看"缓冲区此刻为空"**：刚 `open()` 时所有缓冲区本来就是空的，
"主线程恰好把帧取空"也会短暂成立。误判的代价有两个，都很严重：

1. `masterClock()` 会当场返回被冻结的 `extPts_`（`position` 永远停在 0），
   而 `pumpFrame()` 只取 `pts <= master` 的帧 → **永远取不出帧 → 黑屏**；
2. 主循环会立刻发 `media.eof` / `player.completed` → 自动续播类插件以为片子放完了。

`vDrained_` / `aDrained_` 只在两处置真（解码线程处理完 drain 包之后），
只在两处置假：`open()` 与 `flushAfterSeek()`。**新增跨媒体状态时，
请同时把它加进这两处的复位清单** —— 这是本文件里最容易被漏的一条。

### 2026-09-12 的事故：漏了 `demuxFinished_` / `drainSent_`

`open()` 当时复位了 App 层的 `lastUploadedIndex_` / `completedFired_` 与 Player 层的
时钟、队列、计数器，**唯独漏了这两个**。后果：第一个媒体到 EOF 后它们为真，
第二次 `open()` 之后

```
demuxLoop():  if (demuxFinished_) { sleep(20ms); continue; }   ← 一个包都不读
finished():   立刻为真（缓冲区刚清空）                          ← 时钟冻在 0
```

→ **同一进程里换片必然永久黑屏**（控制通道 `player.open`、插件 `open_media`）。
因为 App 层状态是复位了的，表面看"该清的都清了"，自检又只打开一次媒体，
所以这个缺陷长期没被碰到。现在由 `--selftest` 的 `reopen_media` 步骤钉住
（它会二次打开同一媒体并解码 10 帧；故意去掉那两行复位时该项立刻报 0 帧红）。

## 5. 事件分发模式（对齐 Cordis）

| 模式 | 语义 | 本项目用途 |
|---|---|---|
| `EMIT` | 广播，忽略返回值 | `osd.draw`、`app.tick`、`media.opened`、`audio.samples` |
| `WATERFALL` | 顺序调用，可改写负载/短路 | `video.frame`（帧处理链，可原地改像素 / 丢帧） |
| `SERIAL` | 顺序调用，返回首个非零结果 | 预留（策略类事件） |
| `BAIL` | 顺序调用，遇第一个 `consumed` 即停 | `ui.key`、`ui.mouse`（插件可吞按键） |

主题 → 模式的映射在 `src/core/eventbus.cpp:defaultModeOf`。
`video.frame` 通过 `subscribe(PP_TOPIC_VIDEO_FRAME, ...)` 或
`set_frame_processor()` 注册，两者进的是**同一条链**。

**订阅句柄**：`subscribe()` 返回的 `sub_id` 由 `Host` 统一分配
（`Host::registerSub/takeSub`），帧链订阅与事件总线订阅**共用一套 id 空间**。
这是必要的：`FrameChain::nextId_` 与 `EventBus::nextId_` 各自从 1 开始，
若直接把内部 id 返回给插件，同一插件订阅 `video.frame` + 任意普通主题就会拿到
相同句柄，`unsubscribe()` 于是撤错对象。现在两套命名空间被隔离开，
插件只需把返回的句柄原样传回 `unsubscribe()`。
回归测试：`tests/plugin_apitest/plugin.cpp`（命令 `api.check`，返回 0 = 通过）。

## 6. 服务与注册撤销

每个插件加载时分配一个**槽位号（owner）**。`Host` 在调用插件的任何回调前会
用 `OwnerScope` 把 `owner` 切成该插件，于是这期间发生的所有注册都记在它名下：

```cpp
int revoked = host->revokeAll(slot);   // 撤销该插件的全部注册
//  = bus.unsubscribeAllOf + services.removeAllOf
//    + commands.removeAllOf + chain.removeAllOf
//    + 帧订阅适配器 + 订阅句柄表（subMap_）
```

命令执行也一样：`Host::invokeCommand()` 先查出命令的 owner，再切上下文执行，
因此插件命令里的 `config_get/set` 会写到自己 `plugin.<槽位>.*` 的命名空间。

自检里的 `plugin_hot_reload` 一项就是在验证这个，并且**断言数量必须完全不变**：
`重载后 订阅 4->4, 命令 15->15, 服务 2->2, 帧处理器 0->0（无泄漏）`
（0 = 默认没插件占链，见 §6b；断言的是"重载前后相等"，不是某个固定数字）。
（早期版本只断言 `commands > 0`，收集了泄漏证据却从不断言，等于这项检查永远为绿。）

### 6b. 帧处理器要"按需挂载"，不要常驻

`video_effects` 与 `snapshot` 原先在 `on_start` 就注册帧处理器、一直挂到卸载 ——
即使一个效果都没开。2026-09-12 改成**启用才注册、停用即撤销**（各自的 `syncChain` /
`syncStamp`），于是默认状态下**帧链是空的**（自检报告里 `frame_processors: 0`）。

理由不只是"省两次空回调"：**只要链非空，渲染层就必须走"CPU RGBA"这条路**
（插件的契约是可写 RGBA 像素），于是任何"让 GPU 直接吃 YUV"的优化都会被这条路吃掉。
实测过：把渲染换成 NV12 纹理 + GPU 做 YUV→RGB（4K 每帧上传从 33 MB 降到 12.4 MB），
在"链恒非空"的前提下**是净损失**（4K 掉帧 11 → 78）——因为每帧多了一次 NV12→RGBA 物化。
所以"插件不占用帧链"是那条优化的**前置条件**，不是可有可无的优化。

2026-09-12 补记：前提补上之后 NV12 路线**已启用**，并且在同一份二进制上 A/B 测出
决定性收益（4K 上显示帧数 ≈ RGBA 的 2 倍、丢帧从两位数降到 0；CPU/帧 −53%~−73%）。
完整数字见 [README 验证结果 §16](../README.md)；A/B 开关是 `PP_NO_NV12=1`（强制回退 RGBA）。

排查提示：`effect.status` 会输出 `procId`（0 = 当前没占链）；自检报告里的
`registry.frame_processors` 也是这个数。

## 7. 控制通道协议

换行分隔 JSON，监听 `127.0.0.1`（**只绑回环**，不暴露到局域网）。

### 7.1 安全模型（**默认即受保护**）

控制通道能执行任意原生插件命令（写文件、退出进程），所以它不是"裸接口"：

| 机制 | 实现 |
|---|---|
| 只绑回环 | `bind(127.0.0.1)`；且用 `SO_EXCLUSIVEADDRUSE` 而非 `SO_REUSEADDR`，防止其它进程抢占同一端口（第二个实例 bind 会得到 `10048` 并明确报错） |
| **强制令牌认证** | 启动时用 `BCryptGenRandom` 生成 256-bit 令牌，写入 `<exeDir>/config/rpc-token.txt`（可用 `--rpc-token` 指定）。令牌**每次启动都重新生成** |
| 认证前零能力 | 连接后第一条消息必须是 `auth`；未认证前**任何方法都返回错误**（连 `ping` 都不放行），认证失败立即断开。比较用定长时间实现 |
| 报文长度上限 | 单条报文 4 MB，超限断开（避免无上限行缓冲把内存吃干）；换行扫描是增量的，不存在 O(n²) |
| 并发上限 | 同时最多 8 条连接，超出直接拒绝（避免线程/资源耗尽）；`recv`/`send` 都有超时 |
| 事件只给已认证连接 | 未认证连接收不到任何内核事件（早期版本裸连接即收到全部事件流） |
| 请求在主线程执行 | 见 §2「关键约束 3」 |

> 令牌文件在**控制通道真正监听成功之后**才写。否则第二个实例端口被占时会用新令牌
> 覆盖掉正在运行实例的令牌文件，把它所有客户端一起搞坏。

### 7.2 报文

```
→ {"id":1,"method":"auth","params":{"token":"<64 位 hex>"}}        ← 必须是第一条
← {"id":1,"ok":true,"result":{"authenticated":true,"abi":3}}

→ {"id":2,"method":"player.seek","params":{"seconds":12}}
← {"id":2,"ok":true,"result":{"position":12.0}}
← {"event":"app.tick","data":{"position":12.0,"fps":30.0,...}}     （订阅后主动推送）
```

方法一览：`auth`、`ping`、`player.info|stats|play|pause|toggle|stop|seek|volume|mute|speed|open`、
`command.invoke|list`、`service.list`、`plugin.list`、`topic.subscribe`（`"*"` 订阅全部）、`app.quit`、
`player.tracks|select_track`、`audio.devices|set_device`（P1-d：多轨 + 音频设备选择）。

宿主事件统一经 `Host::publishTopic()` 发出：既分发给进程内订阅者，也同步转发给
**已认证的**控制通道客户端。

测试客户端：`tools/rpc_client.py`（会自动读令牌文件；含 23 项检查，其中 4 项是安全回归）。

## 8. 插件 ABI 设计要点

- **纯 C ABI**：结构体 + 函数指针，不用 C++ 虚表 —— 插件可以用 C 写、也可以被其它语言 FFI。
- **插件不链接 FFmpeg / SDL3**：一切能力经 `PP_HostApi` 提供，因此插件很小、
  不存在跟宿主争抢 DLL 版本的问题。实测各插件 DLL 的导入表（`objdump -p`）：

  | 插件 | 大小 | 第三方运行时依赖 |
  |---|---|---|
  | `hotkey_ext.dll` | 39 KB | 无（仅 KERNEL32 + UCRT） |
  | `audio_viz.dll` | 40 KB | 无 |
  | `snapshot.dll` | 40 KB | 无 |
  | `osd_stats.dll` | 41 KB | 无 |
  | `video_effects.dll` | 180 KB | **`libwinpthread-1.dll`**（用了 `<cmath>`/容器，pthread 未完全静态化） |

  也就是说"插件零依赖"基本成立，但 `video_effects` 是例外，发布时必须带上
  `libwinpthread-1.dll`（`build.py` 已经会拷，内核本身也依赖它）。
  `audio_viz` 的 FFT 是自实现、不引入任何第三方库。
- **ABI 版本双向校验**：既查导出的 `pp_plugin_abi_version()`，也查 `PP_Plugin.abi_version`，
  再加 `PP_HostApi.struct_size`，为未来加字段留出向后兼容空间。
- **入口符号用 C 链接**：`PP_PLUGIN_DECL` 内部自带 `extern "C"`，
  否则 C++ 会把 `pp_plugin_query` 名字修饰掉，`GetProcAddress` 找不到。
- **不跨 DLL 边界传所有权**：宿主返回的 `const char*`（配置值等）由宿主持有，
  插件只读不释放；插件要用帧数据必须走 `acquire_frame`（拿副本）或帧处理链（原地改）。
- **OSD 避让画面**：渲染阶段把 letterbox 之后的画面矩形经 `PP_OsdApi.video_rect`
  （ABI v2）暴露给插件；`osd_stats` / `audio_viz` 与宿主的暂停角标据此**优先画进
  上下黑边**，黑边不足时退回原位。两个插件在底部的空间靠约定协调（底部 48px 归
  进度条，见两边同名注释）。渲染几何一律按 `SDL_GetWindowSizeInPixels` 的**渲染像素**
  计算 —— 用窗口逻辑尺寸在高 DPI 下会让右/下侧 OSD 被整块裁掉（实测过的缺陷）。
- **鼠标事件通道（ABI v3）**：内核在 `pollEvents()` 从 SDL 取出
  `SDL_EVENT_MOUSE_MOTION / _BUTTON_DOWN / _BUTTON_UP / _WHEEL`，构造 `PP_MouseEvent`
  （type/x/y/dx/dy/button/clicks/mod/pts）经 `PP_TOPIC_UI_MOUSE`（**BAIL** 模式）派发到总线。
  插件把 `consumed` 置 1 即"吞掉"该事件，内核的"点画面跳转"等内置响应随之让位 ——
  即"插件接管交互，内核退居能力层"。
- **通用叠加层（ABI v3）**：插件经 `PP_OverlayApi`（`set/move/set_visible/remove/clear`）
  推 RGBA8888 位图图层；内核只保管（`SDL_TEXTUREACCESS_STATIC` 纹理）与合成
  （`renderOverlays()`，在 OSD 之上、Present 之下），**不解释内容** —— 与"一切皆插件"红线一致。
  图层按 `owner` 归属；宿主 `Host::revokeAll(owner)` 在插件卸载时调 `overlayClear(owner)`，
  自动清掉它的全部图层，因此"插件卸载不残留画面"由机制而非约定保证。

## 8b. 信任边界与写入白名单

明确一下哪些东西是"可执行代码边界"，哪些是"数据"：

| 边界 | 说明 |
|---|---|
| `plugins/*.dll` | **等于可执行代码**。宿主用 `LoadLibrary` 加载并调用其导出函数 —— 往这个目录丢 DLL 就是给自己装后门。目录取自 `exeDir()/plugins` 且**不递归**，因此不存在"相对 CWD 落 DLL"的问题。当前**不校验签名/哈希**。 |
| `host-js/plugins/*.mjs` | 同样是**可执行代码**，在 Node 进程里全权限运行。 |
| 控制通道 / Web 控制页 | 见 §7。两者都能驱动插件命令，因此都需要认证。 |
| 控制通道令牌文件 | `config/rpc-token.txt`。能读到它就等于能控制播放器 —— 本机同用户进程本就能读，真正的边界是**操作系统用户权限**，不是这个文件。 |

**插件写入白名单**：`save_png` / `write_text_file` 只允许写进白名单目录，
默认只有 `exeDir()`（即 `dist/`）。需要写别处用 `--write-dir <目录>`
（可重复）或 ini 的 `security.write_dirs=路径1;路径2` 追加。
判定由 `util.cpp` 的 `normalizePath()` + `pathIsInside()` 完成：
先用 `GetFullPathNameA` 把 `..`/`.` 折叠掉并做大小写归一，再比较前缀。
越界写入被拒绝并打 `WARN`。

> 为什么放在宿主而不是插件里：插件是不可信代码，策略必须由宿主强制。
> 这样即便某个插件（或经 RPC 触发的插件命令）拿到任意 `path` 参数，
> 也写不出白名单之外。

**网络输入默认拒绝**：`openMedia()` 默认拒绝 `scheme://`（http/https/rtsp/rtmp/file…）
与 UNC 路径（`\\host\share`），因为控制通道和插件都能触发 `openMedia`，
默认放行就等于给本机装了一个"代任意来源出网"的 SSRF 原语，
UNC 还会触发 SMB 认证（NTLM 中继面）。需要时用 `--allow-network-input` 显式开启。

## 9. 构建要点

- 不用 cmake / make：`build.py` 直接调 `g++`，逐个编译、逐个链接，依赖少、排错直观。
- 链接参数（容易踩的坑）：
  - **不能加 `-static`** —— 那会让 ld 去找 `libSDL3.a`/`libav*.a` 静态库而报「找不到」。
    正确写法是只静态化运行时：`-static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic`。
  - 插件用 `-shared` + 同样的运行时段。实测 5 个插件里 **4 个不依赖 `libwinpthread`**，
    唯 `video_effects.dll` 仍会 import `libwinpthread-1.dll`（用 `objdump -p` 可复核）。
  - 内核 import `libwinpthread-1.dll`，另外还 import `bcrypt.dll`
    （控制通道令牌用 `BCryptGenRandom`），打包时从编译器目录一并拷入 `dist/`。
- `dist/` 只带真正用到的 5 个 FFmpeg DLL（`avfilter`/`avdevice` 不带，省约 110MB；
  需要它们的是 ffmpeg.exe 命令行工具，在 `vendor/ffmpeg/bin/` 里）。
