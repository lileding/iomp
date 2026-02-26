#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <assert.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <vector>
#include <future>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include "iomp.h"

// ============================================================
// test helpers
// ============================================================

static int g_pass = 0;
static int g_fail = 0;

#define TEST_ASSERT(cond, fmt, ...) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: " fmt "\n", ##__VA_ARGS__); \
            g_fail++; \
            return; \
        } \
    } while (0)

#define TEST_RUN(fn) \
    do { \
        fprintf(stderr, "[TEST] %s\n", #fn); \
        fn(); \
        if (g_fail == _prev_fail) { \
            g_pass++; \
            fprintf(stderr, "  PASS\n"); \
        } \
        int _prev_fail __attribute__((unused)) = g_fail; \
    } while (0)

// create nonblocking socketpair
static int make_socketpair(int sv[2]) {
    if (socketpair(AF_LOCAL, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == -1) {
        return -1;
    }
    return 0;
}

// simple completion context for C API tests
struct aio_ctx {
    struct iomp_aio aio;
    std::mutex mtx;
    std::condition_variable cv;
    bool done;
    int error;
};

static void aio_on_complete(struct iomp_aio* aio, int error) {
    struct aio_ctx* ctx = (struct aio_ctx*)((char*)aio - offsetof(struct aio_ctx, aio));
    std::lock_guard<std::mutex> lk(ctx->mtx);
    ctx->done = true;
    ctx->error = error;
    ctx->cv.notify_one();
}

static bool aio_wait(struct aio_ctx* ctx, int timeout_ms) {
    std::unique_lock<std::mutex> lk(ctx->mtx);
    return ctx->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                            [ctx]{ return ctx->done; });
}

static void aio_ctx_init(struct aio_ctx* ctx, int fd, void* buf, size_t nbytes) {
    ctx->aio.fildes = fd;
    ctx->aio.buf = buf;
    ctx->aio.nbytes = nbytes;
    ctx->aio.offset = 0;
    ctx->aio.timeout_ms = -1;
    ctx->aio.complete = aio_on_complete;
    ctx->done = false;
    ctx->error = 0;
}

// ============================================================
// C++ async helper using AsyncIO
// ============================================================

class TestIO : public iomp::AsyncIO {
public:
    TestIO(int fd, void* buf, size_t nbytes)
        : iomp::AsyncIO(fd, buf, nbytes) {}

    void complete(int error) noexcept override {
        std::lock_guard<std::mutex> lk(_mtx);
        _done = true;
        _error = error;
        _cv.notify_one();
    }

    bool wait(int timeout_ms) {
        std::unique_lock<std::mutex> lk(_mtx);
        return _cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                            [this]{ return _done; });
    }

    void reset(int fd, void* buf, size_t nbytes) {
        this->fildes = fd;
        this->buf = buf;
        this->nbytes = nbytes;
        this->offset = 0;
        _done = false;
        _error = 0;
    }

    int error() const { return _error; }

private:
    std::mutex _mtx;
    std::condition_variable _cv;
    bool _done = false;
    int _error = 0;
};

// ============================================================
// 1. basic read/write over socketpair
// ============================================================

static void test_basic_read_write() {
    iomp::IOMultiPlexer mp(2);
    TEST_ASSERT((bool)mp, "IOMultiPlexer create failed");

    int sv[2];
    TEST_ASSERT(make_socketpair(sv) == 0, "socketpair failed");

    char wbuf[128];
    char rbuf[128];
    memset(wbuf, 'A', sizeof(wbuf));
    memset(rbuf, 0, sizeof(rbuf));

    TestIO writer(sv[1], wbuf, sizeof(wbuf));
    TestIO reader(sv[0], rbuf, sizeof(rbuf));

    mp.read(&reader);
    mp.write(&writer);

    TEST_ASSERT(writer.wait(2000), "write timed out");
    TEST_ASSERT(writer.error() == 0, "write error: %d", writer.error());
    TEST_ASSERT(reader.wait(2000), "read timed out");
    TEST_ASSERT(reader.error() == 0, "read error: %d", reader.error());
    TEST_ASSERT(memcmp(rbuf, wbuf, sizeof(wbuf)) == 0, "data mismatch");

    close(sv[0]);
    close(sv[1]);
}

// ============================================================
// 2. small data (1 byte)
// ============================================================

static void test_single_byte() {
    iomp::IOMultiPlexer mp(1);
    int sv[2];
    TEST_ASSERT(make_socketpair(sv) == 0, "socketpair failed");

    char wb = 0x42;
    char rb = 0;

    TestIO writer(sv[1], &wb, 1);
    TestIO reader(sv[0], &rb, 1);

    mp.read(&reader);
    mp.write(&writer);

    TEST_ASSERT(writer.wait(2000), "write timed out");
    TEST_ASSERT(reader.wait(2000), "read timed out");
    TEST_ASSERT(rb == 0x42, "data mismatch: got 0x%02x", (unsigned char)rb);

    close(sv[0]);
    close(sv[1]);
}

// ============================================================
// 3. large data (triggers partial I/O / EAGAIN)
// ============================================================

static void test_large_data() {
    iomp::IOMultiPlexer mp(2);
    int sv[2];
    TEST_ASSERT(make_socketpair(sv) == 0, "socketpair failed");

    const size_t SIZE = 256 * 1024; // 256KB - likely exceeds socket buffer
    char* wbuf = new char[SIZE];
    char* rbuf = new char[SIZE];
    for (size_t i = 0; i < SIZE; i++) {
        wbuf[i] = (char)(i & 0xff);
    }
    memset(rbuf, 0, SIZE);

    TestIO writer(sv[1], wbuf, SIZE);
    TestIO reader(sv[0], rbuf, SIZE);

    mp.read(&reader);
    mp.write(&writer);

    TEST_ASSERT(writer.wait(5000), "write timed out");
    TEST_ASSERT(writer.error() == 0, "write error: %d", writer.error());
    TEST_ASSERT(reader.wait(5000), "read timed out");
    TEST_ASSERT(reader.error() == 0, "read error: %d", reader.error());
    TEST_ASSERT(memcmp(rbuf, wbuf, SIZE) == 0, "data mismatch on large transfer");

    delete[] wbuf;
    delete[] rbuf;
    close(sv[0]);
    close(sv[1]);
}

// ============================================================
// 4. multiple concurrent connections
// ============================================================

static void test_concurrent_connections() {
    iomp::IOMultiPlexer mp(4);
    const int NCONN = 20;
    const size_t SIZE = 4096;

    struct conn {
        int sv[2];
        char wbuf[SIZE];
        char rbuf[SIZE];
        TestIO* writer;
        TestIO* reader;
    };

    std::vector<conn*> conns;
    for (int i = 0; i < NCONN; i++) {
        conn* c = new conn;
        TEST_ASSERT(make_socketpair(c->sv) == 0, "socketpair failed at %d", i);
        memset(c->wbuf, (char)(i + 1), SIZE);
        memset(c->rbuf, 0, SIZE);
        c->writer = new TestIO(c->sv[1], c->wbuf, SIZE);
        c->reader = new TestIO(c->sv[0], c->rbuf, SIZE);
        conns.push_back(c);
    }

    // submit all at once
    for (auto* c : conns) {
        mp.read(c->reader);
        mp.write(c->writer);
    }

    // wait all
    bool ok = true;
    for (int i = 0; i < NCONN; i++) {
        auto* c = conns[i];
        if (!c->writer->wait(5000) || c->writer->error() != 0) {
            fprintf(stderr, "  FAIL: conn %d write failed\n", i);
            ok = false;
        }
        if (!c->reader->wait(5000) || c->reader->error() != 0) {
            fprintf(stderr, "  FAIL: conn %d read failed\n", i);
            ok = false;
        }
        if (memcmp(c->rbuf, c->wbuf, SIZE) != 0) {
            fprintf(stderr, "  FAIL: conn %d data mismatch\n", i);
            ok = false;
        }
    }
    TEST_ASSERT(ok, "concurrent connections had failures");

    for (auto* c : conns) {
        close(c->sv[0]);
        close(c->sv[1]);
        delete c->writer;
        delete c->reader;
        delete c;
    }
}

// ============================================================
// 5. read from closed peer (EOF → error = -1)
// ============================================================

static void test_read_eof() {
    iomp::IOMultiPlexer mp(1);
    int sv[2];
    TEST_ASSERT(make_socketpair(sv) == 0, "socketpair failed");

    char rbuf[64];
    memset(rbuf, 0, sizeof(rbuf));
    TestIO reader(sv[0], rbuf, sizeof(rbuf));

    close(sv[1]); // close write end → reader gets EOF

    mp.read(&reader);

    TEST_ASSERT(reader.wait(2000), "read timed out");
    TEST_ASSERT(reader.error() != 0, "expected error on EOF, got 0");

    close(sv[0]);
}

// ============================================================
// 6. write to closed peer (EPIPE/SIGPIPE)
// ============================================================

static void test_write_epipe() {
    signal(SIGPIPE, SIG_IGN);

    iomp::IOMultiPlexer mp(1);
    int sv[2];
    TEST_ASSERT(make_socketpair(sv) == 0, "socketpair failed");

    close(sv[0]); // close read end

    char wbuf[64];
    memset(wbuf, 'X', sizeof(wbuf));
    TestIO writer(sv[1], wbuf, sizeof(wbuf));

    mp.write(&writer);

    TEST_ASSERT(writer.wait(2000), "write timed out");
    TEST_ASSERT(writer.error() != 0, "expected error on EPIPE, got 0");

    close(sv[1]);
}

// ============================================================
// 7. C API: iomp_read with NULL aio → no crash
// ============================================================

static void test_null_aio() {
    iomp_t mp = iomp_new(1);
    TEST_ASSERT(mp != NULL, "iomp_new failed");

    // should not crash
    iomp_read(mp, NULL);
    iomp_write(mp, NULL);
    iomp_accept(mp, NULL);

    iomp_drop(mp);
}

// ============================================================
// 8. C API: iomp_read with NULL buf → complete(EINVAL)
// ============================================================

static void test_null_buf() {
    iomp_t mp = iomp_new(1);
    TEST_ASSERT(mp != NULL, "iomp_new failed");

    struct aio_ctx ctx;
    aio_ctx_init(&ctx, -1, NULL, 64);

    iomp_read(mp, &ctx.aio);

    TEST_ASSERT(aio_wait(&ctx, 2000), "callback not called");
    TEST_ASSERT(ctx.error == EINVAL, "expected EINVAL, got %d", ctx.error);

    iomp_drop(mp);
}

// ============================================================
// 9. C API: iomp_accept with non-NULL buf → complete(EINVAL)
// ============================================================

static void test_accept_with_buf() {
    iomp_t mp = iomp_new(1);
    TEST_ASSERT(mp != NULL, "iomp_new failed");

    char buf[64];
    struct aio_ctx ctx;
    aio_ctx_init(&ctx, -1, buf, sizeof(buf));

    iomp_accept(mp, &ctx.aio);

    TEST_ASSERT(aio_wait(&ctx, 2000), "callback not called");
    TEST_ASSERT(ctx.error == EINVAL, "expected EINVAL, got %d", ctx.error);

    iomp_drop(mp);
}

// ============================================================
// 10. multiple threads (thread count = 1, 2, 8)
// ============================================================

static void test_thread_counts() {
    int counts[] = { 1, 2, 8 };
    for (int nthread : counts) {
        iomp::IOMultiPlexer mp(nthread);
        TEST_ASSERT((bool)mp, "IOMultiPlexer(%d) create failed", nthread);

        int sv[2];
        TEST_ASSERT(make_socketpair(sv) == 0, "socketpair failed");

        char wbuf[256], rbuf[256];
        memset(wbuf, (char)nthread, sizeof(wbuf));
        memset(rbuf, 0, sizeof(rbuf));

        TestIO writer(sv[1], wbuf, sizeof(wbuf));
        TestIO reader(sv[0], rbuf, sizeof(rbuf));

        mp.read(&reader);
        mp.write(&writer);

        TEST_ASSERT(writer.wait(2000), "write timed out (nthread=%d)", nthread);
        TEST_ASSERT(reader.wait(2000), "read timed out (nthread=%d)", nthread);
        TEST_ASSERT(memcmp(rbuf, wbuf, sizeof(wbuf)) == 0,
                    "data mismatch (nthread=%d)", nthread);

        close(sv[0]);
        close(sv[1]);
    }
}

// ============================================================
// 11. create and immediately drop (no I/O)
// ============================================================

static void test_create_drop() {
    iomp_t mp = iomp_new(2);
    TEST_ASSERT(mp != NULL, "iomp_new failed");
    iomp_drop(mp);
    // no crash = pass
}

// ============================================================
// 12. drop NULL iomp → no crash
// ============================================================

static void test_drop_null() {
    iomp_drop(NULL);
    // no crash = pass
}

// ============================================================
// 13. repeated read/write on same connection
// ============================================================

static void test_repeated_io() {
    iomp::IOMultiPlexer mp(2);
    int sv[2];
    TEST_ASSERT(make_socketpair(sv) == 0, "socketpair failed");

    const int ROUNDS = 50;
    for (int i = 0; i < ROUNDS; i++) {
        char wbuf[64], rbuf[64];
        memset(wbuf, (char)i, sizeof(wbuf));
        memset(rbuf, 0, sizeof(rbuf));

        TestIO writer(sv[1], wbuf, sizeof(wbuf));
        TestIO reader(sv[0], rbuf, sizeof(rbuf));

        mp.read(&reader);
        mp.write(&writer);

        TEST_ASSERT(writer.wait(2000), "write timed out at round %d", i);
        TEST_ASSERT(reader.wait(2000), "read timed out at round %d", i);
        TEST_ASSERT(memcmp(rbuf, wbuf, sizeof(wbuf)) == 0,
                    "data mismatch at round %d", i);
    }

    close(sv[0]);
    close(sv[1]);
}

// ============================================================
// 14. accept on listening socket
// ============================================================

class TestAcceptor : public iomp::AsyncIO {
public:
    TestAcceptor(int fd)
        : iomp::AsyncIO(fd, nullptr, 0) {}

    void complete(int error) noexcept override {
        if (error != 0) {
            std::lock_guard<std::mutex> lk(_mtx);
            _error = error;
            _done = true;
            _cv.notify_one();
            return;
        }
        while (1) {
            int remote = accept(this->fildes, nullptr, nullptr);
            if (remote == -1) {
                break;
            }
            std::lock_guard<std::mutex> lk(_mtx);
            _accepted.push_back(remote);
            _done = true;
            _cv.notify_one();
        }
    }

    bool wait(int timeout_ms) {
        std::unique_lock<std::mutex> lk(_mtx);
        return _cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                            [this]{ return _done; });
    }

    int error() const { return _error; }
    std::vector<int>& accepted() { return _accepted; }

private:
    std::mutex _mtx;
    std::condition_variable _cv;
    bool _done = false;
    int _error = 0;
    std::vector<int> _accepted;
};

static void test_accept() {
    iomp::IOMultiPlexer mp(2);

    // create listening socket
    struct addrinfo hint = {};
    hint.ai_family = AF_INET;
    hint.ai_socktype = SOCK_STREAM;
    hint.ai_flags = AI_PASSIVE;
    struct addrinfo* ai = nullptr;
    TEST_ASSERT(getaddrinfo("127.0.0.1", "0", &hint, &ai) == 0, "getaddrinfo failed");

    int lfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    TEST_ASSERT(lfd >= 0, "socket failed");

    int reuseaddr = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr));
    TEST_ASSERT(bind(lfd, ai->ai_addr, ai->ai_addrlen) == 0, "bind failed");
    TEST_ASSERT(listen(lfd, 16) == 0, "listen failed");
    fcntl(lfd, F_SETFL, fcntl(lfd, F_GETFL, 0) | O_NONBLOCK);

    // get assigned port
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    getsockname(lfd, (struct sockaddr*)&addr, &addrlen);

    freeaddrinfo(ai);

    TestAcceptor acceptor(lfd);
    iomp_accept(mp, &acceptor);

    // connect a client
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(cfd >= 0, "client socket failed");
    TEST_ASSERT(connect(cfd, (struct sockaddr*)&addr, addrlen) == 0, "connect failed");

    TEST_ASSERT(acceptor.wait(2000), "accept timed out");
    TEST_ASSERT(acceptor.error() == 0, "accept error: %d", acceptor.error());
    TEST_ASSERT(acceptor.accepted().size() >= 1, "no accepted connections");

    for (int fd : acceptor.accepted()) {
        close(fd);
    }
    close(cfd);
    close(lfd);
}

// ============================================================
// 15. C++ IOMultiPlexer null pointer throws
// ============================================================

static void test_cpp_null_throws() {
    iomp::IOMultiPlexer mp(1);
    bool caught = false;
    try {
        mp.read((iomp::AsyncIO*)nullptr);
    } catch (const std::invalid_argument&) {
        caught = true;
    }
    TEST_ASSERT(caught, "expected invalid_argument for read(nullptr)");

    caught = false;
    try {
        mp.write((iomp::AsyncIO*)nullptr);
    } catch (const std::invalid_argument&) {
        caught = true;
    }
    TEST_ASSERT(caught, "expected invalid_argument for write(nullptr)");

    caught = false;
    try {
        mp.accept((iomp::AsyncIO*)nullptr);
    } catch (const std::invalid_argument&) {
        caught = true;
    }
    TEST_ASSERT(caught, "expected invalid_argument for accept(nullptr)");
}

// ============================================================
// 16. C++ IOMultiPlexer move semantics
// ============================================================

static void test_cpp_move() {
    iomp::IOMultiPlexer mp1(2);
    TEST_ASSERT((bool)mp1, "mp1 create failed");

    iomp::IOMultiPlexer mp2(std::move(mp1));
    TEST_ASSERT(!(bool)mp1, "mp1 should be empty after move");
    TEST_ASSERT((bool)mp2, "mp2 should be valid after move");

    // mp2 should still work
    int sv[2];
    TEST_ASSERT(make_socketpair(sv) == 0, "socketpair failed");

    char wbuf[32] = "hello move";
    char rbuf[32] = {};
    TestIO writer(sv[1], wbuf, sizeof(wbuf));
    TestIO reader(sv[0], rbuf, sizeof(rbuf));

    mp2.read(&reader);
    mp2.write(&writer);

    TEST_ASSERT(writer.wait(2000), "write timed out");
    TEST_ASSERT(reader.wait(2000), "read timed out");
    TEST_ASSERT(memcmp(rbuf, wbuf, sizeof(wbuf)) == 0, "data mismatch after move");

    close(sv[0]);
    close(sv[1]);
}

// ============================================================
// main
// ============================================================

int main() {
    signal(SIGPIPE, SIG_IGN);
    iomp_loglevel(IOMP_LOGLEVEL_WARNING); // quiet during tests

    fprintf(stderr, "===== iomp unit tests =====\n\n");

    int _prev_fail = 0;

    TEST_RUN(test_basic_read_write);
    TEST_RUN(test_single_byte);
    TEST_RUN(test_large_data);
    TEST_RUN(test_concurrent_connections);
    TEST_RUN(test_read_eof);
    TEST_RUN(test_write_epipe);
    TEST_RUN(test_null_aio);
    TEST_RUN(test_null_buf);
    TEST_RUN(test_accept_with_buf);
    TEST_RUN(test_thread_counts);
    TEST_RUN(test_create_drop);
    TEST_RUN(test_drop_null);
    TEST_RUN(test_repeated_io);
    TEST_RUN(test_accept);
    TEST_RUN(test_cpp_null_throws);
    TEST_RUN(test_cpp_move);

    fprintf(stderr, "\n===== results: %d passed, %d failed =====\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
