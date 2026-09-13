/*
 * player_plugin.h — PluginPlayer 插件 SDK（纯 C ABI，v1）
 * ---------------------------------------------------------------------------
 * 设计目标
 *   1. 插件与宿主之间只依赖 C ABI（struct + 函数指针表），不使用 C++ 虚表，
 *      因此插件可以用 C / C++ 编写，也可以由其它语言通过 FFI 生成。
 *   2. 插件不需要链接 FFmpeg / SDL3：一切能力通过 HostApi 提供。
 *      这样插件很小、不会和宿主产生 DLL 版本冲突。
 *   3. 概念上对齐 Cordis 的「服务 + 事件 + 可撤销注册」模型（见 docs/PLUGIN_GUIDE.md）：
 *        - 服务      -> host->register_service / get_service
 *        - 事件      -> host->subscribe / publish（emit）/ dispatch（waterfall·serial·bail）
 *        - 可撤销注册 -> 每个 register/subscribe 都返回句柄，on_unload 时统一撤销
 *
 * 线程约定（**重要，早期版本这里写反了**）
 *   - 插件的**所有回调都在主线程**执行：on_load / on_start / on_stop / on_unload、
 *     帧处理链（video.frame）、事件分发（audio.samples / osd.draw / ui.key …）、
 *     命令回调，全部跑在同一个 60Hz 主循环线程上。
 *     解码线程**不会**调用任何插件 —— 解码结果由主循环取出后再分发。
 *   - 因此：回调内**不得阻塞**（整条渲染/音频链路都在等你）；插件内部状态
 *     无需加锁（同一时刻只有一个线程进回调）。
 *   - HostApi 面向主线程：**不要从自己起的线程里调用 host->***。
 *     需要异步干活就自己起线程、把结果放进队列，在主线程回调里消费。
 *     连接控制通道的外部请求也会被投递到主线程串行执行，同样是这个约定。
 */
#ifndef PLAYER_PLUGIN_H
#define PLAYER_PLUGIN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * ABI 与版本演进规则（**改这个头文件之前请先读完这一段**）
 * ===========================================================================
 *
 * 1) 唯一的契约是 `abi_version` 的**严格相等**
 *    宿主检查三个地方：插件导出的 `pp_plugin_abi_version()`、
 *    `PP_Plugin.abi_version`、以及 `PP_HostApi.struct_size`（宿主自己那份）。
 *    任何一处不符即拒绝加载。**不做部分兼容。**
 *
 * 2) 因此：对下面任何一个结构体做**任何**字段改动（哪怕只是追加一个字段），
 *    都必须同时递增 `PP_ABI_VERSION`，并**重新编译全部插件**。
 *    涉及的结构体：
 *      - PP_HostApi      （宿主填，插件读；已有 struct_size 字段）
 *      - PP_OsdApi       （宿主填，插件读；追加字段同样必须递增版本并重编插件）
 *      - PP_Plugin       （插件填，宿主读）
 *      - PP_MediaInfo    （插件分配，宿主填充）
 *      - PP_PlaybackStats（插件分配，宿主填充）
 *      - PP_EventCtx     （宿主在栈上构造后传给插件）
 *
 * 3) 为什么 PP_MediaInfo / PP_PlaybackStats / PP_EventCtx **没有** struct_size：
 *    它们是"宿主填充"的 out 参数，**由插件分配**。宿主无法在不越界的前提下
 *    知道插件实际分配了多大（插件通常写 `PP_MediaInfo mi{};`，此时 size 字段
 *    会是 0）。所以在这三个结构上放 size 字段并不能提供真实保护 ——
 *    真正的保护只能是"版本严格相等"。
 *
 * 4) 如果将来确实需要"宿主比插件新也能跑"的部分兼容，正确做法是：
 *      ① 让**调用方**先把 `struct_size` 写成 `sizeof(自己这份)`；
 *      ② 宿主据此裁剪写入（只写落在 sizeof 之内的字段），并允许 struct_size
 *         **小于**宿主的 sizeof；
 *      ③ 同时为零初始化的情况保留一条兼容路径（struct_size == 0 视为旧版）。
 *    这需要改掉**所有**调用点（插件里 7 处以上），收益有限、回归面不小，
 *    因此当前版本刻意不做 —— 记录在此，供将来决策。
 *
 * 5) 当前状态：`PP_ABI_VERSION = 3`，宿主与插件必须来自同一份头文件。
 *    构建脚本 `build.py` 会一起编译宿主与全部插件，所以这在发布流程里是自然的。
 *    v2 的变更：`PP_OsdApi` 追加 `video_rect`（OSD 避让画面黑边，见该字段注释）。
 *    v3 的变更：新增 `PP_MouseEvent`（鼠标事件负载）与 `PP_OverlayApi`
 *              （插件自定义位图叠加层），`PP_HostApi` 追加 `overlay` 字段 ——
 *              追加在**末尾**，旧插件靠 `struct_size` 判断字段可用性（见上文第 2 节）。
 * ===========================================================================
 */

/* ===========================================================================
 * 版本与导出宏
 * =========================================================================== */

#define PP_ABI_VERSION 3u

/** 插件必须导出的入口符号名（见文件末 PP_PLUGIN_DECL）。 */
#define PP_PLUGIN_ENTRY "pp_plugin_query"

#if defined(_WIN32)
#  define PP_DLL_EXPORT __declspec(dllexport)
#else
#  define PP_DLL_EXPORT __attribute__((visibility("default")))
#endif

/* 入口符号必须以 C 链接导出，否则 C++ 会做名字修饰，宿主 GetProcAddress 找不到。 */
#ifdef __cplusplus
#  define PP_LINKAGE_C extern "C"
#else
#  define PP_LINKAGE_C
#endif

/** 声明插件入口。用法（C++）：
 *      static PP_Plugin g_plugin = { ... };
 *      PP_PLUGIN_DECL(&g_plugin);
 * 注意：必须写在全局作用域，宏内部会自带 extern "C"。
 */
#define PP_PLUGIN_DECL(p)                                                        \
    PP_LINKAGE_C PP_DLL_EXPORT const PP_Plugin *pp_plugin_query(void) { return (p); } \
    PP_LINKAGE_C PP_DLL_EXPORT uint32_t pp_plugin_abi_version(void) { return PP_ABI_VERSION; }

/* ===========================================================================
 * 基础类型
 * =========================================================================== */

/** 不透明宿主句柄。插件持有的其实就是个指针，只能喂给 HostApi。 */
typedef struct PP_Host PP_Host;

/** 颜色：0xAABBGGRR（与 SDL 的 RGBA8888 小端布局一致，便于直接喂给渲染层）。 */
typedef uint32_t PP_Color;

#define PP_RGB(r, g, b)      ((PP_Color)(0xFFu << 24 | (uint32_t)(b) << 16 | (uint32_t)(g) << 8 | (uint32_t)(r)))
#define PP_RGBA(r, g, b, a)  ((PP_Color)((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(g) << 8 | (uint32_t)(r)))

enum PP_LogLevel {
    PP_LOG_TRACE = 0,
    PP_LOG_DEBUG = 1,
    PP_LOG_INFO  = 2,
    PP_LOG_WARN  = 3,
    PP_LOG_ERROR = 4,
};

/** 像素格式（当前仅暴露一种，插件只处理 RGBA8888）。
 *
 * 内存字节序：**R, G, B, A** —— 写像素时 q[0]=R、q[1]=G、q[2]=B、q[3]=A。
 * 别被 PP_Color 的 0xAABBGGRR 写法带偏：那是把整行当 uint32 小端读时的值，
 * 与「按字节索引」的顺序相反。灰度权重 q[0]*77 + q[1]*150 + q[2]*29 只有在
 * q[0]=R 时才成立（可对照 src/plugins/video_effects/plugin.cpp）。 */
enum PP_PixelFormat {
    PP_PIXFMT_RGBA8888 = 1,
};

/** 帧处理链返回值（waterfall 语义）。 */
enum PP_ChainResult {
    PP_CHAIN_CONTINUE = 0, /* 交给链上后续处理器 */
    PP_CHAIN_BAIL     = 1, /* 终止链，但照常显示 */
    PP_CHAIN_DROP     = 2, /* 丢弃该帧，不显示 */
};

/** 事件分发模式。 */
enum PP_DispatchMode {
    PP_DISPATCH_EMIT      = 0, /* 广播，忽略返回值 */
    PP_DISPATCH_WATERFALL = 1, /* 环绕中间件，可包装/短路 */
    PP_DISPATCH_SERIAL    = 2, /* 顺序执行，返回首个非零结果 */
    PP_DISPATCH_BAIL      = 3, /* 顺序执行，遇到第一个「已消费」即停 */
};

/** 事件主题（字符串常量，插件也可自定义私有主题）。 */
#define PP_TOPIC_APP_READY      "app.ready"
#define PP_TOPIC_APP_QUIT       "app.quit"
#define PP_TOPIC_APP_TICK       "app.tick"
#define PP_TOPIC_MEDIA_OPENED   "media.opened"
#define PP_TOPIC_MEDIA_CLOSED   "media.closed"
#define PP_TOPIC_MEDIA_EOF      "media.eof"
#define PP_TOPIC_PLAY_STARTED   "player.started"
#define PP_TOPIC_PLAY_PAUSED    "player.paused"
#define PP_TOPIC_PLAY_RESUMED   "player.resumed"
#define PP_TOPIC_PLAY_SEEKED    "player.seeked"
#define PP_TOPIC_PLAY_STOPPED   "player.stopped"
#define PP_TOPIC_PLAY_ERROR     "player.error"
#define PP_TOPIC_PLAY_COMPLETED "player.completed"
#define PP_TOPIC_VIDEO_FRAME    "video.frame"   /* WATERFALL：可改写像素 / 丢弃帧 */
#define PP_TOPIC_AUDIO_SAMPLES  "audio.samples" /* EMIT：只读 PCM 观察 */
#define PP_TOPIC_UI_KEY         "ui.key"        /* BAIL：可吞掉按键 */
#define PP_TOPIC_UI_MOUSE       "ui.mouse"      /* BAIL */
#define PP_TOPIC_UI_RESIZE      "ui.resize"     /* EMIT */
#define PP_TOPIC_OSD_DRAW       "osd.draw"      /* EMIT：插件在此绘制叠加层 */

/* ===========================================================================
 * 数据结构
 * =========================================================================== */

/** 媒体信息快照。字符串均为定长，宿主负责填充。 */
typedef struct PP_MediaInfo {
    char   path[512];
    char   format[64];
    char   video_codec[64];
    char   audio_codec[64];
    int    width;
    int    height;
    double fps;
    double duration;
    int64_t bitrate;
    int    audio_channels;
    int    audio_sample_rate;
    int    has_video;
    int    has_audio;
} PP_MediaInfo;

/** 实时播放统计。 */
typedef struct PP_PlaybackStats {
    double  position;         /* 秒 */
    double  duration;         /* 秒 */
    double  master_clock;
    double  video_clock;
    double  audio_clock;
    double  current_fps;      /* 最近一秒实际显示帧率 */
    double  speed;
    int     paused;
    int     eof;
    int     dropped_frames;
    int64_t decoded_frames;
    int64_t displayed_frames;
    int64_t video_queue_depth;
    int64_t audio_buffer_bytes;
    int     audio_sample_rate;
    int     audio_channels;
} PP_PlaybackStats;

/** 一帧 RGBA8888 像素的可写视图。
 *  data 指向首行首像素；第 y 行第 x 像素 = data + y*pitch + x*4。
 *  **不要假设 pitch == width*4**（可能带行对齐填充），一律用 pitch 步进。 */
typedef struct PP_FrameView {
    uint8_t       *data;    /* RGBA8888 像素，pitch 可能 > width*4 */
    int            width;
    int            height;
    int            pitch;   /* 每行字节数 */
    int            format;  /* enum PP_PixelFormat */
    double         pts;     /* 秒 */
    int64_t        index;   /* 第几帧（显示序号） */
    int            serial;  /* 时间轴序号，seek 后递增 */
} PP_FrameView;

/** 事件上下文。回调通过它读取负载、写回结果。 */
typedef struct PP_EventCtx {
    const char    *topic;
    PP_Host       *host;
    void          *user;          /* subscribe 时传入的 user 指针 */
    const char    *json;          /* 负载（JSON 文本），可能为 NULL */
    void          *payload;       /* 二进制负载（audio.samples / video.frame 使用） */
    int            payload_size;
    int            consumed;      /* 回调置 1 表示已吞掉该事件（BAIL 语义）*/
    int            result;        /* 回调写回的整数结果（SERIAL 语义）*/
    int            mode;          /* enum PP_DispatchMode */
} PP_EventCtx;

/** 事件回调。返回值仅 SERIAL 模式有意义；BAIL 模式请置 ctx->consumed。 */
typedef int (*PP_EventFn)(PP_EventCtx *ctx);

/** audio.samples 事件的负载：最近一小段音频（只读）。 */
typedef struct PP_AudioTap {
    const float *samples;     /* 归一化到 [-1,1]，多声道交错 */
    int          count;       /* 样本总个数（含所有声道） */
    int          sample_rate;
    int          channels;
    double       pts;
} PP_AudioTap;

/** ui.key 事件的负载。吞掉按键请置 ctx->consumed = 1（BAIL 语义）。 */
typedef struct PP_KeyEvent {
    int keycode;     /* SDL_Keycode */
    int scancode;    /* SDL_Scancode */
    int keymod;      /* SDL_Keymod 位掩码 */
    int down;        /* 1 = 按下, 0 = 抬起 */
    int repeat;
} PP_KeyEvent;

/** 命令回调：cmd("seek", ["+5"]) 之类。返回 0 表示成功。 */
typedef int (*PP_CommandFn)(PP_Host *host, int argc, const char **argv, void *user);

/** 帧处理器。可原地改写 view->data；返回值见 enum PP_ChainResult。 */
typedef int (*PP_FrameFn)(PP_Host *host, PP_FrameView *view, void *user);

/* ===========================================================================
 * OSD 绘制 API
 * ---------------------------------------------------------------------------
 * 坐标系：左上角原点，单位为「输出像素」，范围 = 窗口客户区尺寸。
 * 所有调用只在 osd.draw 事件（或 PP_TOPIC_OSD_DRAW 回调）期间有效。
 * =========================================================================== */
typedef struct PP_OsdApi {
    /** 文字。**默认走真字体**（stb_truetype 解析随包 TTF；可用 `PP_FONT` 环境变量
     *  换字体），支持任意 UTF-8 含中文；字体缺失时才回退 SDL 内置 8x8 点阵字，
     *  那种情况下非 ASCII 会以 '?' 呈现。两种情况下 `text_width` 都返回真实像素宽度。 */
    void  (*text)(PP_Host *host, float x, float y, const char *utf8, PP_Color color, float scale);
    float (*text_width)(PP_Host *host, const char *utf8, float scale);
    float (*line_height)(PP_Host *host, float scale);
    /** 半透明面板 + 边框，常用于信息条底板。 */
    void  (*panel)(PP_Host *host, float x, float y, float w, float height,
                   PP_Color bg, PP_Color border);
    void  (*fill_rect)(PP_Host *host, float x, float y, float w, float height, PP_Color color);
    void  (*rect)(PP_Host *host, float x, float y, float w, float height,
                  PP_Color color, float thickness);
    void  (*line)(PP_Host *host, float x1, float y1, float x2, float y2,
                  PP_Color color, float thickness);
    /** 把 values[0..count-1]（各元素 0..1）画成柱状/频谱。 */
    void  (*bars)(PP_Host *host, float x, float y, float w, float height,
                  const float *values, int count, PP_Color color);
    /** OSD 画布尺寸（= 窗口客户区像素）。返回 0 表示当前不可绘制。 */
    int   (*size)(PP_Host *host, float *out_w, float *out_h);
    /** 当前视频画面在画布里的矩形（letterbox 之后的位置与尺寸）。
     *  插件据此把叠加层画进上下/左右黑边，避免遮挡画面；无画面时返回 0。 */
    int   (*video_rect)(PP_Host *host, float *out_x, float *out_y,
                        float *out_w, float *out_h);
} PP_OsdApi;

/* ===========================================================================
 * P1-b 鼠标事件（PP_TOPIC_UI_MOUSE 的负载）
 * ===========================================================================
 * 分发模式是 **BAIL**：插件返回非 0 即"吞掉"该事件，宿主不再执行内置响应
 * （当前内置响应只有"点击进度条跳转"），语义与 `PP_TOPIC_UI_KEY` 一致。
 * 主题的分发模式在内核里已登记（eventbus.cpp 的 topic 表），插件不必再声明。 */
typedef struct PP_MouseEvent {
    int    type;      /* 0=移动 1=按下 2=抬起 3=滚轮 */
    float  x, y;      /* 窗口客户区坐标（像素，左上角为原点） */
    float  dx, dy;    /* 移动：相对上次位移；滚轮：滚动量（y 正为向前） */
    int    button;    /* 1=左 2=中 3=右（按下/抬起有效，其余为 0） */
    int    clicks;    /* 连击数（按下有效） */
    int    mod;       /* 修饰键位掩码：1=Shift 2=Ctrl 4=Alt */
    double pts;       /* 事件发生时的主时钟位置（秒），便于插件做与时间相关的交互 */
} PP_MouseEvent;

/* 便于插件书写（枚举不是 ABI 的一部分，数值才是） */
enum { PP_MOUSE_MOTION = 0, PP_MOUSE_DOWN = 1, PP_MOUSE_UP = 2, PP_MOUSE_WHEEL = 3 };
enum { PP_MOD_SHIFT = 1, PP_MOD_CTRL = 2, PP_MOD_ALT = 4 };

/* ===========================================================================
 * P1-b 通用叠加层
 * ===========================================================================
 * 让插件把自己的**位图**叠到画面上（字幕、弹幕、水印、缩略图预览都建在这上面），
 * 内核只负责保管与合成，不解释内容 —— 与"一切皆插件"的架构红线一致。
 *
 * 生命周期：`set` 返回的 id 由内核分配；**插件卸载时该插件的图层会被自动清除**
 * （Host::revokeAll 按 owner 清理），所以正常路径下插件不必显式 remove；
 * 显式 remove/hide 用于运行期切换。
 *
 * 像素格式固定 RGBA8888、行优先，pitch == w*4（不接受到处传 pitch 的复杂度 ——
 * 这里的图层尺寸通常远小于视频帧，按紧密排布拷贝一次即可）。 */
typedef struct PP_OverlayApi {
    /** 新建或更新一个图层的像素与摆放。id 传 0 表示新建（返回新 id）。
     *  rgba 为 w*h*4 字节，会被**拷贝**（内核持有自己的纹理）。
     *  dw/dh <= 0 时按图像原始尺寸摆放。返回 id（>0），失败返回 0。 */
    int   (*set)(PP_Host *h, int id, const uint8_t *rgba, int w, int hgt,
                 float x, float y, float dw, float dh, float alpha);
    /** 只改摆放/透明度，不动像素（每帧更新的场合用它，避免重复拷像素）。 */
    int   (*move)(PP_Host *h, int id, float x, float y, float dw, float dh, float alpha);
    int   (*set_visible)(PP_Host *h, int id, int visible);
    int   (*remove)(PP_Host *h, int id);
    /** 移除当前插件（调用方）的全部图层。 */
    int   (*clear)(PP_Host *h);
} PP_OverlayApi;

/* ===========================================================================
 * 宿主能力表
 * =========================================================================== */
typedef struct PP_HostApi {
    uint32_t     abi_version;
    uint32_t     struct_size;     /* 宿主填入 sizeof(PP_HostApi)，插件可据此判断字段可用性 */

    /* -- 日志 ---------------------------------------------------------------- */
    void        (*log)(PP_Host *h, int level, const char *fmt, ...);

    /* -- 播放控制 ------------------------------------------------------------ */
    int         (*play)(PP_Host *h);
    int         (*pause)(PP_Host *h);
    int         (*toggle_pause)(PP_Host *h);
    int         (*stop)(PP_Host *h);
    int         (*seek_absolute)(PP_Host *h, double seconds);
    int         (*seek_relative)(PP_Host *h, double delta_seconds);
    int         (*set_volume)(PP_Host *h, float v01);
    float       (*get_volume)(PP_Host *h);
    int         (*set_muted)(PP_Host *h, int muted);
    int         (*is_muted)(PP_Host *h);
    int         (*set_speed)(PP_Host *h, double speed);
    int         (*is_paused)(PP_Host *h);
    double      (*get_position)(PP_Host *h);
    double      (*get_duration)(PP_Host *h);
    int         (*open_media)(PP_Host *h, const char *path);
    int         (*get_media_info)(PP_Host *h, PP_MediaInfo *out);
    int         (*get_stats)(PP_Host *h, PP_PlaybackStats *out);

    /* -- 事件（对齐 Cordis：emit / waterfall / serial / bail） --------------- */
    /** 订阅。返回 sub_id（>0），失败返回 0。 */
    int         (*subscribe)(PP_Host *h, const char *topic, PP_EventFn fn, void *user);
    void        (*unsubscribe)(PP_Host *h, int sub_id);
    /** EMIT：广播，返回被通知的监听者数量。 */
    int         (*publish)(PP_Host *h, const char *topic, const char *json);
    /** WATERFALL / SERIAL：同步分发，返回链结果。 */
    int         (*dispatch)(PP_Host *h, const char *topic, void *payload, int payload_size);
    /** 查询某主题的注册分发模式。 */
    int         (*topic_mode)(PP_Host *h, const char *topic);
    /**
     * 声明**插件自定义主题**的分发模式（PP_DISPATCH_EMIT / WATERFALL / SERIAL / BAIL）。
     * 不调用的话，非内置主题只能走 EMIT，downstream 的 dispatch() 也就拿不到
     * waterfall/BAIL 语义。注册记在调用它的插件名下，卸载时自动清除。
     *
     * 注意：这是 PP_HostApi 的**新增字段，追加在末尾** —— 插件加载时会校验
     * abi_version 与 struct_size，因此旧插件（用旧头文件编译）不受影响。
     */
    int         (*set_topic_mode)(PP_Host *h, const char *topic, int mode);

    /* -- 服务（对齐 Cordis 的 ctx.<key>） ------------------------------------ */
    int         (*register_service)(PP_Host *h, const char *name, const char *version, void *iface);
    void        (*unregister_service)(PP_Host *h, const char *name);
    void       *(*get_service)(PP_Host *h, const char *name);
    /** 枚举已注册服务名，写入 out（每项 PP_SERVICE_NAME_MAX 字节）。返回写入数量。 */
    int         (*list_services)(PP_Host *h, char *out, int max_names, int stride);

    /* -- 命令 ---------------------------------------------------------------- */
    int         (*register_command)(PP_Host *h, const char *name, const char *usage,
                                    PP_CommandFn fn, void *user);
    void        (*unregister_command)(PP_Host *h, const char *name);
    int         (*invoke_command)(PP_Host *h, const char *name, int argc, const char **argv);
    /** 把命令表（每行 "name  usage  来源插件"）写入 out。返回行数。 */
    int         (*list_commands)(PP_Host *h, char *out, int out_size);

    /* -- 帧访问与处理链 ------------------------------------------------------ */
    /** 取当前显示帧的可写副本视图；成功后必须 release_frame。 */
    int         (*acquire_frame)(PP_Host *h, PP_FrameView *out);
    void        (*release_frame)(PP_Host *h, PP_FrameView *view);
    /** 注册帧处理器（waterfall 链，priority 小的先执行）。返回 processor_id。 */
    int         (*set_frame_processor)(PP_Host *h, PP_FrameFn fn, void *user, int priority);
    void        (*remove_frame_processor)(PP_Host *h, int processor_id);

    /* -- OSD ---------------------------------------------------------------- */
    const PP_OsdApi *osd;
    /* v3：通用位图叠加层（字幕/弹幕/水印/预览的底座）。
     * 追加在 osd 之后；旧插件用 struct_size 判断该字段是否可用。 */
    const PP_OverlayApi *overlay;

    /* -- 配置持久化（config/pluginplayer.ini） ------------------------------- */
    const char  *(*config_get)(PP_Host *h, const char *key, const char *def);
    int         (*config_set)(PP_Host *h, const char *key, const char *value);

    /* -- 杂项 ---------------------------------------------------------------- */
    const char  *(*app_dir)(PP_Host *h);
    const char  *(*plugin_dir)(PP_Host *h);
    const char  *(*asset_dir)(PP_Host *h);
    uint64_t     (*ticks_ms)(PP_Host *h);
    void        (*request_quit)(PP_Host *h);
    /** 当前窗口标题后缀（插件可用来显示状态，传入 NULL 清除）。 */
    void        (*set_window_subtitle)(PP_Host *h, const char *text);
    /** 把一块 RGBA8888 像素写成 PNG。宿主内置实现，插件无需链接任何图像库。 */
    int         (*save_png)(PP_Host *h, const char *path, const uint8_t *rgba,
                            int width, int height, int pitch);
    /** 把 UTF-8 文本写入文件（导出/日志用）。 */
    int         (*write_text_file)(PP_Host *h, const char *path, const char *utf8);
} PP_HostApi;

#define PP_SERVICE_NAME_MAX 64

/* ===========================================================================
 * 插件描述
 * =========================================================================== */
typedef struct PP_Plugin {
    uint32_t     abi_version;   /* 必须 == PP_ABI_VERSION */
    const char  *name;
    const char  *version;
    const char  *author;
    const char  *description;
    uint32_t     capabilities;  /* 保留位，暂未使用 */

    /** 宿主加载后立即调用。应在此注册服务/命令/订阅事件。
     *  返回非 0 表示加载失败，宿主将卸载该插件。 */
    int         (*on_load)(PP_Host *host, const PP_HostApi *api);
    /** 全部插件加载完毕后调用，可开始工作。 */
    int         (*on_start)(PP_Host *host);
    /** 卸载前调用：停止工作、释放资源（此时注册项仍然有效）。 */
    void        (*on_stop)(PP_Host *host);
    /** 最后调用：宿主此时已撤销该插件的全部注册项。 */
    void        (*on_unload)(PP_Host *host);
} PP_Plugin;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PLAYER_PLUGIN_H */
