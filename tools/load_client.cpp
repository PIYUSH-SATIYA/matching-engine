// load_client HOST PORT N [seed]
// Sends N random orders via TCP in wire format and prints throughput.

#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <random>
#include <sys/socket.h>
#include <unistd.h>

#pragma pack(push, 1)
struct WireMsg {
    uint64_t id;
    uint8_t  side;
    uint8_t  pad[7];
    int64_t  price;
    uint64_t quantity;
};
#pragma pack(pop)

static_assert(sizeof(WireMsg) == 32, "");

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "Usage: load_client HOST PORT N [seed]\n");
        return 1;
    }

    const char* host = argv[1];
    uint16_t    port = static_cast<uint16_t>(std::atoi(argv[2]));
    uint64_t    N    = static_cast<uint64_t>(std::atoll(argv[3]));
    unsigned    seed = (argc >= 5) ? static_cast<unsigned>(std::atoi(argv[4])) : 42u;

    // Connect
    addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", port);
    if (::getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        std::fprintf(stderr, "getaddrinfo failed\n");
        return 1;
    }

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { std::perror("socket"); return 1; }

    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        std::perror("connect"); return 1;
    }
    ::freeaddrinfo(res);

    // Build messages in batches
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> price_noise(0.0, 20.0);
    std::uniform_int_distribution<uint64_t> qty_dist(1, 100);
    std::uniform_int_distribution<int> side_dist(0, 1);

    static constexpr std::size_t BATCH = 1024;
    WireMsg batch[BATCH];

    auto t_start = std::chrono::steady_clock::now();

    uint64_t sent = 0;
    while (sent < N) {
        std::size_t chunk = (N - sent < BATCH) ? static_cast<std::size_t>(N - sent) : BATCH;
        for (std::size_t i = 0; i < chunk; ++i) {
            WireMsg& m = batch[i];
            m.id       = sent + i + 1;
            m.side     = static_cast<uint8_t>(side_dist(rng));
            std::memset(m.pad, 0, 7);
            double p = 10000.0 + price_noise(rng);
            m.price    = static_cast<int64_t>(std::round(p));
            m.quantity = qty_dist(rng);
        }
        const char* ptr = reinterpret_cast<const char*>(batch);
        std::size_t total = chunk * sizeof(WireMsg);
        while (total > 0) {
            ssize_t r = ::send(fd, ptr, total, 0);
            if (r <= 0) { std::perror("send"); ::close(fd); return 1; }
            ptr   += r;
            total -= static_cast<std::size_t>(r);
        }
        sent += chunk;
    }

    auto t_end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t_end - t_start).count();
    std::printf("Sent %llu orders in %.3f s = %.0f orders/sec\n",
                (unsigned long long)N, elapsed, N / elapsed);

    ::close(fd);
    return 0;
}
