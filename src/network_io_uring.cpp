// io_uring (completion-based) network ingestion.
//
// Same wire format and same INetworkServer contract as the epoll backend.
// Single-shot ACCEPT/RECV operation, one completion reaped per CQE.  This is
// deliberately the simple, correct version; multishot/SQPOLL are left out
// because they change the mechanism being compared.

#include "network_layer.hpp"

#ifdef ME_HAS_IO_URING

#include "wire.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr int kMaxConns = 4096;
constexpr int kBufSize = 4096;
constexpr std::uint32_t kAccept = 1;
constexpr std::uint32_t kRecv = 2;

struct Conn {
    int fd{-1};
    std::size_t used{0};
    char buf[kBufSize];
};

void setNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

std::uint64_t makeUserData(std::uint32_t type, int fd) {
    return (static_cast<std::uint64_t>(type) << 32) |
           static_cast<std::uint32_t>(fd);
}

class UringServer final : public INetworkServer {
 public:
    UringServer(std::uint16_t port, PushFn push, void* ctx,
                bool exit_on_disconnect)
        : port_(port), push_(push), ctx_(ctx),
          exit_on_disconnect_(exit_on_disconnect) {}

    ~UringServer() override {
        if (ring_ready_) io_uring_queue_exit(&ring_);
    }

    bool start() override {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;

        int opt = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        ::setsockopt(listen_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(listen_fd_, 128) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        setNonBlocking(listen_fd_);

        if (io_uring_queue_init(1024, &ring_, 0) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        ring_ready_ = true;
        for (int i = 0; i < kMaxConns; ++i) conns_[i].fd = -1;

        submitAccept();
        if (io_uring_submit(&ring_) < 0) return false;
        ++stats_.submits;
        return true;
    }

    void run(std::atomic<bool>& stop) override {
        if (!ring_ready_) return;

        struct __kernel_timespec ts {
            0, 100'000'000  // 100 ms
        };

        while (!stop.load(std::memory_order_relaxed)) {
            struct io_uring_cqe* waited = nullptr;
            int ret = io_uring_wait_cqe_timeout(&ring_, &waited, &ts);
            if (ret == -ETIME) continue;
            if (ret == -EINTR) continue;
            if (ret < 0) break;

            struct io_uring_cqe* cqes[64];
            unsigned n = io_uring_peek_batch_cqe(&ring_, cqes, 64);
            for (unsigned i = 0; i < n; ++i) {
                handle(cqes[i]);
                io_uring_cqe_seen(&ring_, cqes[i]);
                ++stats_.completions;
            }

            if (io_uring_submit(&ring_) >= 0) ++stats_.submits;
            if (exit_on_disconnect_ && had_conn_ && open_conns_ == 0) break;
        }

        for (int i = 0; i < kMaxConns; ++i) {
            if (conns_[i].fd >= 0) {
                ::close(conns_[i].fd);
                conns_[i].fd = -1;
            }
        }
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    NetStats stats() const override { return stats_; }

 private:
    struct io_uring_sqe* getSqe() {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (sqe == nullptr) {
            io_uring_submit(&ring_);
            ++stats_.submits;
            sqe = io_uring_get_sqe(&ring_);
        }
        return sqe;
    }

    void submitAccept() {
        struct io_uring_sqe* sqe = getSqe();
        if (!sqe) return;
        io_uring_prep_accept(sqe, listen_fd_, nullptr, nullptr, 0);
        io_uring_sqe_set_data64(sqe, makeUserData(kAccept, listen_fd_));
    }

    void submitRecv(int fd) {
        Conn& c = conns_[fd];
        struct io_uring_sqe* sqe = getSqe();
        if (!sqe) return;
        io_uring_prep_recv(sqe, fd, c.buf + c.used, kBufSize - c.used, 0);
        io_uring_sqe_set_data64(sqe, makeUserData(kRecv, fd));
        ++stats_.recv_calls;
    }

    void handle(struct io_uring_cqe* cqe) {
        const std::uint64_t ud = io_uring_cqe_get_data64(cqe);
        const std::uint32_t type = static_cast<std::uint32_t>(ud >> 32);
        const int fd = static_cast<int>(static_cast<std::uint32_t>(ud));
        const int res = cqe->res;

        if (type == kAccept) {
            if (res >= 0) {
                const int cfd = res;
                setNonBlocking(cfd);
                int nodelay = 1;
                ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                             sizeof(nodelay));
                if (cfd >= 0 && cfd < kMaxConns) {
                    conns_[cfd].fd = cfd;
                    conns_[cfd].used = 0;
                    had_conn_ = true;
                    ++open_conns_;
                    ++stats_.accepts;
                    submitRecv(cfd);
                } else {
                    ::close(cfd);
                }
            }
            if (!(res == -ECANCELED || res == -EBADF)) submitAccept();
            return;
        }

        if (fd < 0 || fd >= kMaxConns) return;
        Conn& c = conns_[fd];
        if (res <= 0) {
            if (res == -EAGAIN || res == -EINTR) {
                submitRecv(fd);
            } else {
                ::close(fd);
                c.fd = -1;
                c.used = 0;
                if (open_conns_ > 0) --open_conns_;
            }
            return;
        }

        stats_.bytes += static_cast<std::uint64_t>(res);
        c.used += static_cast<std::size_t>(res);
        c.used = wire::parse(c.buf, c.used, stats_.invalid,
                             [&](const wire::Msg& m) {
                                 Order o = wire::toOrder(m);
                                 if (m.client_ts_ns == 0)
                                     o.submit_time = std::chrono::steady_clock::now();
                                 push_(ctx_, o);
                             });
        submitRecv(fd);
    }

    std::uint16_t port_;
    PushFn push_;
    void* ctx_;
    bool exit_on_disconnect_{false};
    bool had_conn_{false};
    int open_conns_{0};
    int listen_fd_{-1};
    struct io_uring ring_ {};
    bool ring_ready_{false};
    Conn conns_[kMaxConns];
    NetStats stats_;
};

}  // namespace

std::unique_ptr<INetworkServer> makeUringServer(std::uint16_t port, PushFn push,
                                                void* ctx,
                                                bool exit_on_disconnect) {
    return std::make_unique<UringServer>(port, push, ctx, exit_on_disconnect);
}

#else  // !ME_HAS_IO_URING

std::unique_ptr<INetworkServer> makeUringServer(std::uint16_t, PushFn, void*,
                                                bool) {
    return nullptr;
}

#endif
