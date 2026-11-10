#include "../include/network_layer.hpp"
#include "../include/order.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>

// Wire format: 32 bytes, little-endian, packed
// u64 id; u8 side; u8 pad[7]; i64 price; u64 quantity
static constexpr int MSG_SIZE = 32;

#pragma pack(push, 1)
struct WireMsg {
    uint64_t id;
    uint8_t  side;
    uint8_t  pad[7];
    int64_t  price;
    uint64_t quantity;
};
#pragma pack(pop)

static_assert(sizeof(WireMsg) == 32, "WireMsg must be 32 bytes");

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct ConnBuf {
    static constexpr int BUF_SIZE = 4096;
    char buf[BUF_SIZE];
    int  used{0};
    int  fd{-1};
};

NetworkServer::NetworkServer(uint16_t port, void(*push)(void*, const Order&), void* ctx)
    : port_(port), push_(push), ctx_(ctx) {}

bool NetworkServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::setsockopt(listen_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 128) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    set_nonblocking(listen_fd_);
    return true;
}

void NetworkServer::run(std::atomic<bool>& stop) {
    if (listen_fd_ < 0) return;

    int epfd = ::epoll_create1(0);
    if (epfd < 0) return;

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    ::epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd_, &ev);

    // Simple per-fd buffer map using a flat array (no heap allocation per conn)
    static constexpr int MAX_CONNS = 4096;
    static ConnBuf conn_bufs[MAX_CONNS];
    // Initialize
    for (int i = 0; i < MAX_CONNS; ++i) conn_bufs[i].fd = -1;

    epoll_event events[64];

    while (!stop.load(std::memory_order_relaxed)) {
        int n = ::epoll_wait(epfd, events, 64, 100); // 100ms timeout
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd_) {
                // Accept new connection
                while (true) {
                    sockaddr_in caddr{};
                    socklen_t clen = sizeof(caddr);
                    int cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&caddr), &clen);
                    if (cfd < 0) break;
                    set_nonblocking(cfd);
                    int nodelay = 1;
                    ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

                    if (cfd < MAX_CONNS) {
                        conn_bufs[cfd].fd = cfd;
                        conn_bufs[cfd].used = 0;
                        epoll_event cev{};
                        cev.events = EPOLLIN | EPOLLET;
                        cev.data.fd = cfd;
                        ::epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                    } else {
                        ::close(cfd);
                    }
                }
            } else {
                // Data on client fd
                ConnBuf& cb = conn_bufs[fd];
                bool disconnected = false;

                while (true) {
                    int space = ConnBuf::BUF_SIZE - cb.used;
                    if (space == 0) {
                        // Buffer full, process what we have then drain
                        cb.used = 0;
                        break;
                    }
                    ssize_t r = ::recv(fd, cb.buf + cb.used, space, 0);
                    if (r == 0) { disconnected = true; break; }
                    if (r < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        disconnected = true;
                        break;
                    }
                    cb.used += static_cast<int>(r);

                    // Process complete messages
                    int offset = 0;
                    while (cb.used - offset >= MSG_SIZE) {
                        WireMsg msg;
                        std::memcpy(&msg, cb.buf + offset, MSG_SIZE);
                        offset += MSG_SIZE;

                        // Validate
                        if (msg.side > 1 || msg.quantity == 0) {
                            ++invalid_count_;
                            continue;
                        }

                        Order order{};
                        order.id = msg.id;
                        order.side = (msg.side == 0) ? Side::Buy : Side::Sell;
                        order.price = msg.price;
                        order.quantity = msg.quantity;
                        order.submit_time = std::chrono::steady_clock::now();
                        push_(ctx_, order);
                    }

                    // Move leftover bytes to front
                    int remaining = cb.used - offset;
                    if (remaining > 0 && offset > 0) {
                        std::memmove(cb.buf, cb.buf + offset, remaining);
                    }
                    cb.used = remaining;
                }

                if (disconnected) {
                    ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    ::close(fd);
                    conn_bufs[fd].fd = -1;
                    conn_bufs[fd].used = 0;
                }
            }
        }
    }

    // Cleanup
    for (int i = 0; i < MAX_CONNS; ++i) {
        if (conn_bufs[i].fd >= 0) {
            ::close(conn_bufs[i].fd);
            conn_bufs[i].fd = -1;
        }
    }
    ::close(listen_fd_);
    ::close(epfd);
}
