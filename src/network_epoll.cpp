// epoll (readiness-based) network ingestion.
//
// Wire format: include/wire.hpp (40 bytes, client timestamp included).
// One network thread accepts connections and pushes Orders into the queue.

#include "network_layer.hpp"
#include "wire.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr int kMaxConns = 4096;
constexpr int kBufSize = 4096;

struct Conn {
    int fd{-1};
    std::size_t used{0};
    char buf[kBufSize];
};

void setNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

class EpollServer final : public INetworkServer {
 public:
    EpollServer(std::uint16_t port, PushFn push, void* ctx, bool exit_on_disconnect)
        : port_(port), push_(push), ctx_(ctx),
          exit_on_disconnect_(exit_on_disconnect) {}

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
        return true;
    }

    void run(std::atomic<bool>& stop) override {
        if (listen_fd_ < 0) return;

        int epfd = ::epoll_create1(0);
        if (epfd < 0) return;

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = listen_fd_;
        ::epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd_, &ev);

        for (int i = 0; i < kMaxConns; ++i) conns_[i].fd = -1;

        epoll_event events[64];
        while (!stop.load(std::memory_order_relaxed)) {
            ++stats_.epoll_waits;
            int n = ::epoll_wait(epfd, events, 64, 100);
            for (int i = 0; i < n; ++i) {
                const int fd = events[i].data.fd;
                if (fd == listen_fd_) {
                    acceptLoop(epfd);
                } else {
                    serviceConn(epfd, fd);
                }
            }
            if (exit_on_disconnect_ && had_conn_ && open_conns_ == 0) break;
        }

        for (int i = 0; i < kMaxConns; ++i) {
            if (conns_[i].fd >= 0) {
                ::epoll_ctl(epfd, EPOLL_CTL_DEL, conns_[i].fd, nullptr);
                ::close(conns_[i].fd);
                conns_[i].fd = -1;
            }
        }
        ::close(listen_fd_);
        listen_fd_ = -1;
        ::close(epfd);
    }

    NetStats stats() const override { return stats_; }

 private:
    void acceptLoop(int epfd) {
        while (true) {
            sockaddr_in caddr{};
            socklen_t clen = sizeof(caddr);
            ++stats_.accepts;
            int cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&caddr), &clen);
            if (cfd < 0) break;  // EAGAIN on the non-blocking listener
            setNonBlocking(cfd);
            int nodelay = 1;
            ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

            if (cfd >= kMaxConns) {
                ::close(cfd);
                continue;
            }
            conns_[cfd].fd = cfd;
            conns_[cfd].used = 0;
            had_conn_ = true;
            ++open_conns_;

            epoll_event cev{};
            cev.events = EPOLLIN | EPOLLET;
            cev.data.fd = cfd;
            if (::epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev) < 0) {
                ::close(cfd);
                conns_[cfd].fd = -1;
            }
        }
    }

    void serviceConn(int epfd, int fd) {
        if (fd < 0 || fd >= kMaxConns) return;
        Conn& c = conns_[fd];
        bool disconnected = false;

        while (true) {
            const std::size_t space = kBufSize - c.used;
            if (space == 0) {  // should not happen with a valid 40-byte stream
                disconnected = true;
                break;
            }
            ++stats_.recv_calls;
            ssize_t r = ::recv(fd, c.buf + c.used, space, 0);
            if (r == 0) {
                disconnected = true;
                break;
            }
            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                disconnected = true;
                break;
            }
            stats_.bytes += static_cast<std::uint64_t>(r);
            c.used += static_cast<std::size_t>(r);
            c.used = wire::parse(c.buf, c.used, stats_.invalid,
                                 [&](const wire::Msg& m) {
                                     Order o = wire::toOrder(m);
                                     const auto now = std::chrono::steady_clock::now();
                                     if (m.client_ts_ns == 0) o.submit_time = now;
                                     o.enqueue_time = now;
                                     if (ingestStatsEnabled())
                                         noteIngest(
                                             stats_, m.client_ts_ns,
                                             std::chrono::duration_cast<
                                                 std::chrono::nanoseconds>(
                                                 now.time_since_epoch())
                                                 .count());
                                     push_(ctx_, o);
                                 });
        }

        if (disconnected) {
            ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
            ::close(fd);
            if (fd < kMaxConns) {
                conns_[fd].fd = -1;
                conns_[fd].used = 0;
                if (open_conns_ > 0) --open_conns_;
            }
        }
    }

    std::uint16_t port_;
    PushFn push_;
    void* ctx_;
    bool exit_on_disconnect_{false};
    bool had_conn_{false};
    int open_conns_{0};
    int listen_fd_{-1};
    Conn conns_[kMaxConns];
    NetStats stats_;
};

}  // namespace

std::unique_ptr<INetworkServer> makeEpollServer(std::uint16_t port, PushFn push,
                                                void* ctx,
                                                bool exit_on_disconnect) {
    return std::make_unique<EpollServer>(port, push, ctx, exit_on_disconnect);
}
