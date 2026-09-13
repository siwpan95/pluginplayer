#pragma once
/* control.h — 宿主控制通道（JSON-RPC over TCP，换行分隔）
 *
 * 作用：让**进程外**的插件层（例如 host-js/ 里的 Cordis）能像进程内插件一样
 * 驱动播放器、查询状态、订阅事件。这样「插件」就有两种形态：
 *   1) 进程内原生插件  plugins/ 下的 .dll —— 走 C ABI，零拷贝、可改像素
 *   2) 进程外 JS 插件   host-js/       —— 走本控制通道，可热重载、生态丰富
 *
 * 协议（每条消息一行 JSON）：
 *   请求: {"id":1,"method":"player.seek","params":{"seconds":12}}
 *   应答: {"id":1,"ok":true,"result":{...}}
 *   事件: {"event":"player.paused","data":{...}}
 *
 * ============================ 安全模型（重要） ============================
 * 控制通道能执行任意插件命令（含写文件、退出进程），因此**默认即受保护**：
 *   · 只监听 127.0.0.1（不暴露到局域网）
 *   · **强制令牌认证**：启动时生成 256-bit 随机令牌（可用 --rpc-token 指定），
 *     写入 <exeDir>/config/rpc-token.txt（同目录 config/ 已限本机）
 *   · 连接建立后的**第一条**消息必须是：
 *         {"id":1,"method":"auth","params":{"token":"<64 位 hex>"}}
 *     认证通过前，任何其它方法一律返回错误；认证失败立即断开连接。
 *   · 单条报文硬上限 kMaxLineBytes，超限断连（防内存耗尽）
 *   · 并发连接上限 kMaxClients（防线程耗尽）
 *   · 空闲超时 kIdleTimeoutMs（**不断连**，仅用于让阻塞的 recv 周期性醒来以便退出；
 *     取小值见下 —— 它同时决定「客户端保持连接时 app.quit 的退出延迟」）
 *   · 事件只推给**已认证**的连接（未认证连接收不到任何内核事件）
 * ==========================================================================
 */
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <memory>
#include <atomic>
#include <condition_variable>
#include <cstdint>

#include "host.h"

namespace pp {

class RpcServer {
public:
    /* ---- 安全参数（硬上限） ---- */
    static constexpr size_t kMaxLineBytes   = 4u * 1024u * 1024u;  /* 单条报文 4 MB */
    static constexpr int    kMaxClients     = 8;                    /* 并发连接上限 */
    /* recv 空闲超时。**不是断连超时**：客户端空闲不会被断开，这个值只是让阻塞在
     * recv 的线程周期性醒来查一次 running_。必须取小 —— 实测 Windows 下跨线程
     * ::shutdown(sk, SD_BOTH) 并不唤醒 pending recv（V1 复现：app.quit 后进程
     * 卡满整整一个超时周期才退出），于是「客户端保持连接时 app.quit 的退出延迟」
     * 就等于这个值。取 1000ms：退出延迟 ≤1s，且 8 个并发连接每秒共 8 次唤醒，
     * 开销可忽略。 */
    static constexpr int    kIdleTimeoutMs  = 1000;                 /* recv 空闲唤醒周期 */
    static constexpr int    kSendTimeoutMs  = 10000;                /* send 超时 */
    static constexpr int    kRequestTimeoutSec = 10;                /* 等主线程执行的上限 */

    ~RpcServer();

    /**
     * port<=0 表示不启动。
     * token 为空时自动生成一个 256-bit 随机令牌（见 generateToken）。
     */
    bool start(Host* host, int port, const std::string& token = std::string());
    void stop();
    bool running() const { return running_.load(); }
    int  port() const { return port_; }
    int  clientCount() const;

    /**
     * 主线程每帧调用一次：把工作线程收到的请求搬到主线程执行。
     *
     * 为什么必须这样做：Player / Host 的大量状态（extPts_、lastAudioClock_、
     * 当前帧、解码器、SDL 音频流…）都不是原子的，只被主循环读写。
     * 早期版本让 RPC 工作线程直接调用 handleRequest，于是 JS 宿主每 5 秒轮询
     * 一次 stats 就会和主循环并发改写这些字段（数据竞争）。
     * 现在所有请求都经由这个队列在主线程串行执行，主循环状态只被一个线程触碰。
     */
    void pumpMainThread();

    /** 本次运行的认证令牌（64 位 hex）。 */
    const std::string& token() const { return token_; }

    /** 生成 256-bit 随机令牌（32 字节 → 64 位小写 hex，使用系统 CSPRNG）。 */
    static std::string generateToken();

    /** 把宿主事件转发给已订阅该主题的**已认证**客户端。
     *  只会在主线程调用（Host::publishTopic 的所有来源都在主线程）。 */
    void broadcastEvent(const std::string& topic, const std::string& json);

private:
    struct Client {
        std::atomic<uintptr_t> sock{(uintptr_t)-1};
        std::thread            th;
        std::mutex             mu;          /* 保护 topics 与 send 的顺序 */
        std::vector<std::string> topics;    /* 空 = 订阅全部 */
        std::atomic<bool>      authed{false};
        std::atomic<bool>      closing{false};

        /* 关键：clientLoop 由工作线程自己执行，而线程函数结束时它持有的
         * 最后一个 shared_ptr 会归零 → 在**仍在运行的线程内部**析构这个 Client。
         * 此时 th 依然 joinable，std::thread 的析构函数会直接 std::terminate
         * （实测症状：客户端一断开，整个播放器 exit 0xC0000409）。
         * 正常退出路径由 RpcServer::stop() 负责 join；这里兜底 detach。 */
        ~Client() {
            if (th.joinable()) th.detach();
        }
    };

    /* 工作线程 → 主线程的请求信封 */
    struct Pending {
        std::string             line;
        std::shared_ptr<Client> client;
        std::string             response;
        bool                    done = false;
        std::mutex              mu;
        std::condition_variable cv;
        std::atomic<bool>       abandoned{false};
    };

    void acceptLoop();
    void clientLoop(std::shared_ptr<Client> c);
    /** 交主线程执行并等待结果；返回 false 表示超时/被放弃。 */
    bool dispatchToMainThread(const std::string& line,
                              const std::shared_ptr<Client>& c,
                              std::string* out);
    std::string handleRequest(const std::string& line, const std::shared_ptr<Client>& c);
    void sendLine(uintptr_t sock, const std::string& s);
    /** 线程安全地给单个客户端发一行（内部取 c->mu）。 */
    void sendLineTo(const std::shared_ptr<Client>& c, const std::string& s);

    Host*                  host_ = nullptr;
    std::atomic<bool>      running_{false};
    int                    port_ = 0;
    uintptr_t              listenSock_ = (uintptr_t)-1;
    std::thread            acceptTh_;
    mutable std::mutex     mu_;
    std::vector<std::shared_ptr<Client>> clients_;
    std::atomic<int64_t>   served_{0};
    std::atomic<int64_t>   authFailures_{0};
    std::string            token_;

    /* 主线程执行队列 */
    mutable std::mutex                     qmu_;
    std::deque<std::shared_ptr<Pending>>   queue_;
};

} // namespace pp
