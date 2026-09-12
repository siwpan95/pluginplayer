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
| 主线程 | SDL 事件循环、`pumpFrame`、帧处理链、渲染、OSD、事件分发 | 插件回调大多在此 |
| demux 线程 | `av_read_frame` → 分派到视频/音频包队列；处理 seek | 通过 `interrupt_callback` 让 seek/abort 能打断阻塞读 |
| 视频解码线程 | 收包解码 → `sws_scale` 转 RGBA → 帧队列（满则等） | `video.frame` 回调也在这条链上 |
| 音频解码线程 | 收包解码 → `swr_convert` 转 S16 → 环形缓冲 | `audio.samples` 的数据源 |
| SDL 音频线程 | 音频回调：从环形缓冲取数 → `SDL_PutAudioStreamData` | 回调里只做搬运，绝不做重活 |
| RPC accept/客户端线程 | 控制通道请求处理 | 调用宿主 API 是线程安全的 |

**关键约束：`AVCodecContext` 非线程安全。**
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

## 3. A/V 同步

- **音频时钟**：`ringBasePts + consumedBytes / bytesPerSec`。
  `consumedBytes` 由音频回调累加，再减去 `SDL_GetAudioStreamQueued`（SDL 内部尚未播出的字节），
  使时钟贴近真实播放位置。
- **主时钟**：有音频且音频时钟在推进 → 用音频时钟；否则用墙钟。
- **停滞保护**：若音频时钟 350ms 内没有推进（典型场景：默认音频设备是**未连接的蓝牙耳机**，
  WASAPI 打开了设备但不出声），自动退回墙钟并同步基准，避免整机播放卡在 0 秒。
- **视频取帧**：`pumpFrame()` 每轮把 `pts <= master + 4ms` 的帧全部弹出，**只保留最后一帧**，
  中间的直接计为 dropped —— 落后时自动跳帧追进度。
- **暂停/变速**：暂停时墙钟冻结、音频回调直接返回（自然静音）；
  变速用 `SDL_SetAudioStreamFrequencyRatio(stream, speed)` 做重采样，视频看主时钟。

## 4. 精确 seek

`AVSEEK_FLAG_BACKWARD` 只能回到**前一个关键帧**。对关键帧稀疏的文件（例如 x264 默认
keyint=250，8 秒视频只有 1 个关键帧），seek 到 4.8s 会实际落在 0s。

处理方式（`dropBefore_`）：seek 后把目标时刻之前的解码输出全部丢弃
（视频帧、音频帧都丢），直到时间戳追上目标；同时主时钟立即乐观置为目标值，
这样用户看到的是「立刻跳到 4.8s」而不是「从头快进」。

实测：目标 4.80s → `seek` 后位置 4.99s（见自检报告）。

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

## 6. 服务与注册撤销

每个插件加载时分配一个**槽位号（owner）**。`Host` 在调用插件的任何回调前会
用 `OwnerScope` 把 `owner` 切成该插件，于是这期间发生的所有注册都记在它名下：

```cpp
int revoked = host->revokeAll(slot);   // 撤销该插件的全部注册
//  = bus.unsubscribeAllOf + services.removeAllOf
//    + commands.removeAllOf + chain.removeAllOf + 帧订阅适配器
```

命令执行也一样：`Host::invokeCommand()` 先查出命令的 owner，再切上下文执行，
因此插件命令里的 `config_get/set` 会写到自己 `plugin.<槽位>.*` 的命名空间。

自检里的 `plugin_hot_reload` 一项就是在验证这个：
重载前后 `订阅 4->4, 命令 15->15, 服务 2->2, 帧处理器 2->2` —— 泄漏为 0。

## 7. 控制通道协议

换行分隔 JSON，监听 `127.0.0.1`（**只绑回环**，不暴露到局域网）。

```
→ {"id":1,"method":"player.seek","params":{"seconds":12}}
← {"id":1,"ok":true,"result":{"position":12.0}}
← {"event":"app.tick","data":{"position":12.0,"fps":30.0,...}}     （订阅后主动推送）
```

方法一览：`ping`、`player.info|stats|play|pause|toggle|stop|seek|volume|mute|speed|open`、
`command.invoke|list`、`service.list`、`plugin.list`、`topic.subscribe`（`"*"` 订阅全部）、`app.quit`。

宿主事件统一经 `Host::publishTopic()` 发出：既分发给进程内订阅者，也**同步转发**给控制通道客户端。

## 8. 插件 ABI 设计要点

- **纯 C ABI**：结构体 + 函数指针，不用 C++ 虚表 —— 插件可以用 C 写、也可以被其它语言 FFI。
- **插件不链接 FFmpeg / SDL3**：一切能力经 `PP_HostApi` 提供，插件 DLL 只有 ~76KB，
  不存在跟宿主争抢 DLL 版本的问题（唯一的例外是 `video_effects` 用了 `std::vector`/`<cmath>`，约 470KB）。
- **ABI 版本双向校验**：既查导出的 `pp_plugin_abi_version()`，也查 `PP_Plugin.abi_version`，
  再加 `PP_HostApi.struct_size`，为未来加字段留出向后兼容空间。
- **入口符号用 C 链接**：`PP_PLUGIN_DECL` 内部自带 `extern "C"`，
  否则 C++ 会把 `pp_plugin_query` 名字修饰掉，`GetProcAddress` 找不到。
- **不跨 DLL 边界传所有权**：宿主返回的 `const char*`（配置值等）由宿主持有，
  插件只读不释放；插件要用帧数据必须走 `acquire_frame`（拿副本）或帧处理链（原地改）。

## 9. 构建要点

- 不用 cmake / make：`build.py` 直接调 `g++`，逐个编译、逐个链接，依赖少、排错直观。
- 链接参数（容易踩的坑）：
  - **不能加 `-static`** —— 那会让 ld 去找 `libSDL3.a`/`libav*.a` 静态库而报「找不到」。
    正确写法是只静态化运行时：`-static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic`。
  - 插件用 `-shared` + 同样的运行时段，于是 `plugins/*.dll` **不依赖 libwinpthread**。
  - 内核仍会 import `libwinpthread-1.dll`，打包时从编译器目录一并拷入 `dist/`。
- `dist/` 只带真正用到的 5 个 FFmpeg DLL（`avfilter`/`avdevice` 不带，省约 110MB；
  需要它们的是 ffmpeg.exe 命令行工具，在 `vendor/ffmpeg/bin/` 里）。
