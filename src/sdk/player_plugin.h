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
 * 线程约定
 *   - on_load / on_start / on_stop / on_unload 都在主线程调用。
 *   - 事件回调可能在解码线程（video.frame / audio.samples）或主线程（其余）触发。
 *     回调内**不得**阻塞、不得长时间做重活；需要重活请自行起线程。
 *   - host->* 的播放控制函数是线程安全的（内部加锁）。
 */
#ifndef PLAYER_PLUGIN_H
#define PLAYER_PLUGIN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * 版本与导出宏
 * =========================================================================== */

#define PP_ABI_VERSION 1u

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

/** 像素格式（当前仅暴露一种，插件只处理 RGBA8888）。 */
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

/** 一帧 RGBA8888 像素的可写视图。 */
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
    /** 文字（内置 8x8 点阵字体，支持可打印 ASCII；非 ASCII 以 '?' 呈现）。 */
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
} PP_OsdApi;

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
