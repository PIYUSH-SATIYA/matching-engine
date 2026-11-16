# 02 — Network layer: epoll and io_uring

The network layer owns sockets and parsing. It creates an `Order` and gives it
to the queue. It never asks the book for prices and never creates trades.

## Common contract

Both implementations expose the same factory interface:

```cpp
std::unique_ptr<INetworkServer> makeEpollServer(...);
std::unique_ptr<INetworkServer> makeUringServer(...);
```

**Source:** `include/network_layer.hpp:45-60`.

The common callback is:

```cpp
using PushFn = void (*)(void*, const Order&);
```

**Source:** `include/network_layer.hpp:53`.

The network server knows only that it can call `push_(ctx_, order)`. The context
is actually the selected queue object, but that wiring is done by `main.cpp`.

## epoll implementation

Startup:

```text
socket -> SO_REUSEADDR/TCP_NODELAY -> bind -> listen
-> non-blocking listener -> epoll_ctl(ADD listener)
```

**Source:** `src/network_epoll.cpp:43-75`.

Runtime:

```cpp
int n = epoll_wait(epfd, events, 64, 100);
for each event:
    if listener:
        acceptLoop(epfd);
    else:
        serviceConn(epfd, fd);
```

**Source:** `src/network_epoll.cpp:79-92`.

For a connection, edge-triggered mode means the implementation drains until
`EAGAIN`:

```cpp
while (true) {
    ssize_t r = recv(fd, buffer + used, space, 0);
    if (r == 0) disconnect;
    if (r < 0 && errno == EAGAIN) break;
    used += r;
    used = wire::parse(...);
}
```

**Source:** `src/network_epoll.cpp:139-177`.

## io_uring implementation

Startup creates an io_uring queue, submits an accept request, and counts the
submission. **Source:** `src/network_io_uring.cpp:81-92`.

Runtime waits for completion queue entries:

```cpp
io_uring_wait_cqe_timeout(...);
io_uring_peek_batch_cqe(...);
for each cqe:
    handle(cqe);
    io_uring_cqe_seen(...);
io_uring_submit(...);
```

**Source:** `src/network_io_uring.cpp:102-118`.

An accept completion registers a connection and submits its first receive:

```cpp
conns_[cfd].fd = cfd;
conns_[cfd].used = 0;
submitRecv(cfd);
```

**Source:** `src/network_io_uring.cpp:168-186`.

A receive completion follows the same parse-and-push path as epoll:

```cpp
c.used += res;
c.used = wire::parse(c.buf, c.used, stats_.invalid, callback);
submitRecv(fd);
```

**Source:** `src/network_io_uring.cpp:190-222`.

## What changes and what does not

```text
epoll      = wait for readiness, then call recv()
io_uring   = submit recv, then handle completion

both       = same 40-byte parser
both       = same Order fields
both       = same enqueue_time point
both       = same queue push callback
```

Changing `--net` is therefore intended to isolate the I/O mechanism rather than
change matching semantics.
