# 插件开发指南

两种插件共享同一套概念：

| 概念 | 原生插件（C ABI） | JS 插件（Cordis） |
|---|---|---|
| 生命周期 | `on_load / on_start / on_stop / on_unload` | `apply(ctx)` + `ctx.effect(() => dispose)` |
| 服务（提供） | `host->register_service(name, ver, iface)` | `class X extends Service { super(ctx,'name') }` |
| 服务（消费） | `host->get_service(name)` | `inject: ['player']` → `ctx.player` |
| 事件订阅 | `host->subscribe(topic, fn, user)` | `ctx.on('player/<topic>', fn)` |
| 事件发布 | `host->publish(topic, json)` | `ctx.emit(...)` / `ctx.player.command(...)` |
| 可撤销注册 | 卸载时 `revokeAll(owner)` 自动撤销全部 | `ctx.effect`/`ctx.on` 自动 dispose |

---

## 一、写一个原生插件（C ABI）

### 1. 最小可编译示例

`examples/hello_plugin/plugin.cpp`：

```cpp
#include "player_plugin.h"
#include <cstdio>

static const PP_HostApi* g_api = nullptr;
static int g_frames = 0;

/* 帧处理链回调：拿到的就是即将显示的像素（RGBA8888，可原地改写） */
static int onFrame(PP_Host* h, PP_FrameView* v, void* user) {
    g_frames++;
    /* 每 60 帧在左上角画一个 32x32 的红块，演示「直接改像素」 */
    if (g_frames % 60 == 0 && v && v->data) {
        for (int y = 0; y < 32 && y < v->height; ++y) {
            uint8_t* row = v->data + (size_t)y * v->pitch;
            for (int x = 0; x < 32 && x < v->width; ++x) {
                uint8_t* q = row + (size_t)x * 4;
                q[0] = 40; q[1] = 40; q[2] = 240; q[3] = 255;   /* BGRA 顺序 */
            }
        }
    }
    return PP_CHAIN_CONTINUE;   /* 也可以 BAIL(1) / DROP(2) */
}

/* 命令：hello.status */
static int cmdStatus(PP_Host* h, int argc, const char** argv, void* user) {
    char buf[128];
    snprintf(buf, sizeof(buf), "hello_plugin: 已处理 %d 帧", g_frames);
    g_api->log(h, PP_LOG_INFO, "%s", buf);
    g_api->set_window_subtitle(h, buf);
    return g_frames;
}

/* 生命周期 */
static int onLoad(PP_Host* host, const PP_HostApi* api) {
    if (api->abi_version != PP_ABI_VERSION || api->struct_size < sizeof(PP_HostApi))
        return -1;                      /* ABI 不匹配，宿主会拒绝加载 */
    g_api = api;
    api->register_command(host, "hello.status", "hello.status  查询处理帧数", &cmdStatus, nullptr);
    api->log(host, PP_LOG_INFO, "hello_plugin 已加载");
    return 0;
}

static int  onStart(PP_Host* host) {
    g_api->set_frame_processor(host, &onFrame, nullptr, 50);  /* 优先级小的先跑 */
    return 0;
}
static void onStop(PP_Host* host)   { g_api->log(host, PP_LOG_INFO, "hello_plugin 停止"); }
static void onUnload(PP_Host* host) { g_api->log(host, PP_LOG_INFO, "hello_plugin 卸载"); }

static PP_Plugin g_plugin = {
    PP_ABI_VERSION, "hello_plugin", "1.0.0", "示例作者",
    "最小原生插件示例：帧处理链 + 注册命令",
    0, &onLoad, &onStart, &onStop, &onUnload,
};

PP_PLUGIN_DECL(&g_plugin)   /* 导出 pp_plugin_query / pp_plugin_abi_version */
```

### 2. 编译

```bat
cd G:\local\player\pluginplayer
F:\mingw64\mingw64\bin\g++.exe -std=c++17 -O2 -shared ^
  -Isrc\sdk ^
  examples\hello_plugin\plugin.cpp -o dist\plugins\hello_plugin.dll ^
  -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic
```

把 DLL 丢进 `dist/plugins/` 即可，下次启动自动加载（无需改动宿主）。
想纳入常规构建，把目录名加进 `build.py` 的 `PLUGINS` 列表（约定源码路径为
`src/plugins/<name>/plugin.cpp`）。

### 3. 必须遵守的约定

1. **入口必须在全局作用域用 `PP_PLUGIN_DECL`**，宏里已经带了 `extern "C"`；
   漏了它符号会被 C++ 修饰，宿主 `GetProcAddress` 找不到。
2. **`abi_version` 必须等于 `PP_ABI_VERSION`**，否则宿主拒绝加载并打印原因。
3. **插件不要链接 FFmpeg / SDL3**。需要什么就用 `PP_HostApi`：
   - 解码信息 → `get_media_info` / `get_stats`
   - 像素 → `set_frame_processor`（原地改，最快）或 `acquire_frame`（拿副本）
   - 想要第三方库（FFT、图像处理…）→ 自己静态编进 DLL
4. **回调里不要阻塞**。帧处理链和事件分发跑在解码/渲染路径上，长时间占用会掉帧。
   需要重活请自己起线程。
5. **插件不携带跨边界所有权**：宿主返回的 `const char*` 只读不释放；
   自己 malloc 的东西自己 free。
6. **注意像素字节序**：`PP_PIXFMT_RGBA8888` 在内存里是
   `B,G,R,A`（小端 0xAABBGGRR），写像素时 q[0]=B、q[2]=R。

### 4. HostApi 速查

| 分类 | 函数 |
|---|---|
| 日志 | `log(host, level, fmt, ...)` |
| 播放控制 | `play/pause/toggle_pause/stop`、`seek_absolute/seek_relative`、`set_volume/get_volume/set_muted/is_muted`、`set_speed/is_paused`、`get_position/get_duration`、`open_media` |
| 状态查询 | `get_media_info(PP_MediaInfo*)`、`get_stats(PP_PlaybackStats*)` |
| 事件 | `subscribe(topic, fn, user)` → sub_id、`unsubscribe(id)`、`publish(topic, json)`、`dispatch(topic, payload, size)`、`topic_mode(topic)` |
| 服务 | `register_service(name, ver, iface)`、`unregister_service`、`get_service(name)`、`list_services` |
| 命令 | `register_command(name, usage, fn, user)`、`unregister_command`、`invoke_command(name, argc, argv)`、`list_commands` |
| 帧 | `set_frame_processor(fn, user, priority)`、`remove_frame_processor(id)`、`acquire_frame/release_frame` |
| OSD | `osd->text / text_width / line_height / panel / fill_rect / rect / line / bars / size` |
| 配置 | `config_get(key, def)`、`config_set(key, value)`（自动落在 `plugin.<槽位>.<键>`） |
| 杂项 | `app_dir/plugin_dir/asset_dir`、`ticks_ms`、`request_quit`、`set_window_subtitle`、`save_png`、`write_text_file` |

`invoke_command` 的 `argv` **只含实参**，不含命令名（`argc=1, argv={"gray"}` 表示一个参数 `gray`）。

### 5. 内置插件里的现成范例

| 想学什么 | 看哪个文件 |
|---|---|
| 事件订阅 + OSD 绘制 + 消费别的插件的服务 | `src/plugins/osd_stats/plugin.cpp` |
| 帧处理链（waterfall）+ 丢帧 + 注册服务 | `src/plugins/video_effects/plugin.cpp` |
| 订阅二进制负载事件 + 自实现算法 | `src/plugins/audio_viz/plugin.cpp` |
| 帧副本 + PNG 写出 + 配置持久化 | `src/plugins/snapshot/plugin.cpp` |
| BAIL（吞按键）+ 调用别的插件的命令 | `src/plugins/hotkey_ext/plugin.cpp` |

### 6. 调试

- 日志在 `dist/logs/pluginplayer.log`，`--log-level 0` 打开 TRACE。
- 插件加载失败的原因会打进日志和控制台（`logs/` 与 `--list-plugins` 报告里都有）。
- 想看插件对画面的影响：`pluginplayer.exe --selftest --frames 30 --out out 视频.mp4`
  会把处理后的帧存成 PNG，用看图工具直接对比。
- 崩溃排查：`g++ -O0 -g` 重编内核后用 `gdb` 跑（`F:\mingw64\mingw64\bin\gdb.exe`）。

---

## 二、写一个 JS 插件（Cordis，进程外）

先给播放器开控制通道并启动 JS 宿主：

```bat
dist\pluginplayer.exe --rpc-port 7911 视频.mp4
cd host-js
npm install
node src\index.mjs --port 7911
```

### 1. 函数式插件：订阅事件

`host-js/plugins/my-plugin.mjs`：

```js
export const config = { threshold: 0.5 }

const apply = (ctx, config) => {
  ctx.logger.info('[my-plugin] 已装载')

  ctx.on('player/media.opened', (d) => {
    ctx.logger.info(`[my-plugin] 打开 ${d?.path}`)
  })

  ctx.on('player/event', ({ topic, data }) => {
    /* 通配订阅 */
  })

  /* 每 5 秒查一次状态，超过阈值就跳回开头 */
  const timer = setInterval(async () => {
    try {
      const st = await ctx.player.stats()
      if (st.duration > 0 && st.position / st.duration > config.threshold) {
        await ctx.player.seek(0)
      }
    } catch { /* 未连接时忽略 */ }
  }, 5000)

  ctx.effect(() => () => clearInterval(timer))   // 卸载时自动清定时器
}

export default { name: 'my-plugin', inject: ['player'], apply }
```

### 2. 服务型插件：既消费又提供服务

```js
import { Service } from 'cordis'

export default class MyService extends Service {
  static inject = ['player']          // 依赖 ctx.player 就绪后才启动
  static name = 'my-service'

  constructor(ctx, config = {}) {
    super(ctx, 'myservice')           // 注册为 ctx.myservice
    ctx.effect(() => () => ctx.logger.info('[my-service] 已卸载'))
  }

  async info() { return ctx.player.info() }
}

export const config = {}
```

> 注意：Cordis 4 里 `ctx.set(name, value)` 需要该服务由**当前插件的 Fiber** 提供，
> 最稳妥的写法就是继承 `Service` 并在构造函数里 `super(ctx, name)`。

### 3. `ctx.player` API

| 方法 | 说明 |
|---|---|
| `call(method, params)` | 直接发原始 JSON-RPC |
| `info()` / `stats()` | 媒体信息 / 实时统计 |
| `play() / pause() / toggle()` | 播放控制 |
| `seek(sec)` / `seekBy(delta)` | 绝对 / 相对跳转 |
| `volume(v) / mute(v) / speed(v)` | 音量 0..1 / 静音 / 倍速 |
| `open(path)` / `command(name, args)` | 换片 / 调用**任意原生插件**的命令 |
| `services()` / `plugins()` | 列出服务 / 插件 |
| `counter` | `{requests, events, errors, reconnects}` |

事件以两种方式派发：`player/event`（通配，带 `{topic,data}`）和
`player/<topic>`（如 `player/media.opened`）。

### 4. 内置 JS 插件范例

| 想学什么 | 看哪个文件 |
|---|---|
| 通配订阅 + `ctx.effect` 落盘 | `host-js/plugins/01-event-log.mjs` |
| 服务型插件 + 状态持久化 + 调用原生命令 | `host-js/plugins/02-auto-resume.mjs` |
| 提供自己的 HTTP UI（零依赖） | `host-js/plugins/03-web-remote.mjs` |

### 5. 调试

- 宿主控制台会打印装载结果与失败原因；`--exit-after N` 可让它跑 N 秒后优雅退出（自检用）。
- 不想启动播放器也能测：`tools/rpc_client.py` 是纯 Python 客户端，可当参考实现。
- 事件没收到？先确认 `topic.subscribe` 成功 —— `PlayerService` 连接后会自动订阅 `"*"`。

---

## 三、什么时候用哪种插件

| 需求 | 选择 | 原因 |
|---|---|---|
| 改画面像素、做滤镜 | 原生 | 帧处理链是零拷贝原地改，JS 做不到 |
| 音频分析 / 频谱 | 原生（`audio.samples` + 自己写算法） | 每帧都要算，跨进程太慢 |
| 叠加信息、画 UI 元素 | 原生（OSD API） | 直接在渲染层画，无延迟 |
| 业务逻辑、断点续播、媒体库 | JS | 改完即生效，不用重编 |
| 远程控制 / 自己的 Web UI | JS | Node 生态，写 HTML 最方便 |
| 需要 npm 上的库 | JS | 原生插件得静态编译进去 |

两者可以混用并且能互相调用：JS 侧 `ctx.player.command('effect.set', ['gray'])`
就是在驱动原生插件（自检里已验证）。
