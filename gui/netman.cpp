#include "netman.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/*
 * BSD-socket HTTP client, see netman.h.
 *
 * Deliberately minimal: an HTTP/1.0 GET with "Connection: close", so
 * the body simply runs to EOF and neither chunked encoding nor
 * keep-alive bookkeeping is needed. Redirects are followed because the
 * download script may hand the file over to another path.
 */

namespace {

/* Socket timeouts. The receive timeout also bounds how long cancel()
 * can have to wait for a stalled server. */
const int IO_TIMEOUT_SEC = 5;
const int CONNECT_TIMEOUT_MS = 5000;
const int MAX_REDIRECTS = 5;
const size_t MAX_HEADER_LINE = 4096;
const int CHUNK = 8192;

struct Url {
    std::string host;
    std::string path;
    int port;
};

std::string lower(const std::string & s)
{
    std::string out(s);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = (char)tolower((unsigned char)out[i]);
    return out;
}

std::string trim(const std::string & s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t'))
        ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' ||
                     s[e - 1] == '\r' || s[e - 1] == '\n'))
        --e;
    return s.substr(b, e - b);
}

/* Only plain http:// is supported; the ROM archive has no TLS. */
bool parse_url(const std::string & url, Url & out, std::string & err)
{
    if (url.compare(0, 7, "http://") != 0) {
        err = (url.compare(0, 8, "https://") == 0)
            ? "HTTPS is not supported" : "not an HTTP URL";
        return false;
    }

    size_t slash = url.find('/', 7);
    if (slash == std::string::npos)
        slash = url.size();

    std::string hostport = url.substr(7, slash - 7);
    out.path = (slash < url.size()) ? url.substr(slash) : "/";
    out.port = 80;

    const size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        out.port = atoi(hostport.c_str() + colon + 1);
        hostport = hostport.substr(0, colon);
    }
    if (hostport.empty() || out.port <= 0 || out.port > 65535) {
        err = "bad host or port";
        return false;
    }

    out.host = hostport;
    return true;
}

/* Turn a possibly relative Location into an absolute URL. */
std::string resolve_location(const std::string & base, const std::string & loc)
{
    if (loc.compare(0, 7, "http://") == 0)
        return loc;

    Url u;
    std::string err;
    if (!parse_url(base, u, err))
        return loc;

    const std::string root = "http://" + u.host +
        (u.port == 80 ? std::string() : ":" + std::to_string(u.port));
    if (!loc.empty() && loc[0] == '/')
        return root + loc;

    /* Relative to the current path: drop its last segment. */
    const size_t slash = u.path.find_last_of('/');
    return root + u.path.substr(0, slash + 1) + loc;
}

/* Blocking-connect with a timeout: a non-blocking connect plus
 * select(), so a dead host cannot stall for the kernel default. */
int tcp_connect(const Url & u, std::string & err)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", u.port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo * res = nullptr;
    const int rc = getaddrinfo(u.host.c_str(), port_str, &hints, &res);
    if (rc != 0) {
        err = std::string("DNS failed: ") + gai_strerror(rc);
        return -1;
    }

    int fd = -1;
    for (struct addrinfo * ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;

        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int c = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (c < 0 && errno == EINPROGRESS) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(fd, &wf);
            struct timeval tv;
            tv.tv_sec = CONNECT_TIMEOUT_MS / 1000;
            tv.tv_usec = (CONNECT_TIMEOUT_MS % 1000) * 1000;
            c = ::select(fd + 1, nullptr, &wf, nullptr, &tv);
            if (c > 0) {
                int so_err = 0;
                socklen_t len = sizeof(so_err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len);
                c = (so_err == 0) ? 0 : -1;
            }
        }

        if (c == 0) {
            fcntl(fd, F_SETFL, flags);    /* back to blocking I/O */
            struct timeval io_tv;
            io_tv.tv_sec = IO_TIMEOUT_SEC;
            io_tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_tv, sizeof(io_tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_tv, sizeof(io_tv));
            freeaddrinfo(res);
            return fd;
        }

        ::close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    err = "cannot connect to " + u.host;
    return -1;
}

/* One GET request positioned right at the start of the body. */
class HttpGet
{
public:
    ~HttpGet() { close(); }

    bool open(const std::string & url, std::string & err);
    long long content_length() const { return length; }
    /* >0 bytes read, 0 = EOF, -1 = error/timeout. */
    int read(uint8_t * buf, int max);
    void close();

private:
    bool recv_line(std::string & line, std::string & err);
    bool send_all(const std::string & data, std::string & err);

    int fd = -1;
    long long length = -1;
};

bool HttpGet::open(const std::string & url, std::string & err)
{
    std::string current = url;

    for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
        Url u;
        if (!parse_url(current, u, err))
            return false;

        close();
        fd = tcp_connect(u, err);
        if (fd < 0)
            return false;

        /* HTTP/1.0 + close: the body ends with the connection, so no
         * chunked decoding is required. */
        const std::string req = "GET " + u.path + " HTTP/1.0\r\n"
                              + "Host: " + u.host + "\r\n"
                              + "User-Agent: Vector06C-TrimUI/1.0\r\n"
                              + "Accept: */*\r\n"
                              + "Connection: close\r\n\r\n";
        if (!send_all(req, err)) {
            close();
            return false;
        }

        std::string line;
        if (!recv_line(line, err)) {
            close();
            return false;
        }
        int status = 0;
        if (sscanf(line.c_str(), "HTTP/%*s %d", &status) != 1) {
            err = "malformed response";
            close();
            return false;
        }

        std::string location;
        length = -1;
        for (;;) {
            if (!recv_line(line, err)) {
                close();
                return false;
            }
            if (line.empty())
                break;                          /* end of headers */
            const size_t colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            const std::string name = lower(line.substr(0, colon));
            const std::string value = trim(line.substr(colon + 1));
            if (name == "content-length")
                length = atoll(value.c_str());
            else if (name == "location")
                location = value;
        }

        if (status == 200)
            return true;

        if ((status == 301 || status == 302 || status == 303 ||
             status == 307 || status == 308) && !location.empty()) {
            current = resolve_location(current, location);
            continue;
        }

        err = "HTTP " + std::to_string(status);
        close();
        return false;
    }

    err = "too many redirects";
    close();
    return false;
}

/* Header lines are read a byte at a time: they are short, and this
 * keeps the body exactly where the socket cursor is (no over-read
 * buffer to carry around). */
bool HttpGet::recv_line(std::string & line, std::string & err)
{
    line.clear();
    for (;;) {
        char c;
        const ssize_t n = ::recv(fd, &c, 1, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            err = (errno == EAGAIN || errno == EWOULDBLOCK)
                ? "timeout" : strerror(errno);
            return false;
        }
        if (n == 0) {
            if (line.empty()) {
                err = "connection closed";
                return false;
            }
            return true;                        /* last line, no CRLF */
        }
        if (c == '\n') {
            if (!line.empty() && line[line.size() - 1] == '\r')
                line.erase(line.size() - 1);
            return true;
        }
        line += c;
        if (line.size() > MAX_HEADER_LINE) {
            err = "header too long";
            return false;
        }
    }
}

bool HttpGet::send_all(const std::string & data, std::string & err)
{
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent,
                                 data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            err = "send failed";
            return false;
        }
        if (n == 0) {
            err = "send failed";
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

int HttpGet::read(uint8_t * buf, int max)
{
    for (;;) {
        const ssize_t n = ::recv(fd, buf, (size_t)max, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        return (int)n;                          /* 0 = EOF */
    }
}

void HttpGet::close()
{
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
    length = -1;
}

} /* anonymous namespace */

/* ---- blocking one-shot GET --------------------------------------- */

bool NetMan::http_get(const std::string & url, std::vector<uint8_t> & out,
                      size_t max_bytes, std::string & err)
{
    out.clear();

    HttpGet get;
    if (!get.open(url, err))
        return false;

    uint8_t chunk[CHUNK];
    for (;;) {
        const int n = get.read(chunk, (int)sizeof(chunk));
        if (n < 0) {
            err = "read failed";
            return false;
        }
        if (n == 0)
            break;
        if (out.size() + (size_t)n > max_bytes) {
            err = "body too large";
            return false;
        }
        out.insert(out.end(), chunk, chunk + n);
    }
    return true;
}

/* ---- background fetch -------------------------------------------- */

NetMan::AsyncFetch::AsyncFetch()
    : running(false), abort_flag(false), success(false),
      got(0), total_bytes(-1)
{
}

NetMan::AsyncFetch::~AsyncFetch()
{
    cancel();
}

void NetMan::AsyncFetch::cancel()
{
    abort_flag.store(true, std::memory_order_relaxed);
    if (th.joinable())
        th.join();
    running.store(false, std::memory_order_relaxed);
}

void NetMan::AsyncFetch::start_memory(const std::string & url, size_t max_bytes)
{
    begin(url, std::string(), max_bytes);
}

void NetMan::AsyncFetch::start_file(const std::string & url,
                                    const std::string & path)
{
    begin(url, path, 0);
}

void NetMan::AsyncFetch::begin(const std::string & url, const std::string & path,
                               size_t max_bytes)
{
    cancel();

    body.clear();
    got.store(0, std::memory_order_relaxed);
    total_bytes.store(-1, std::memory_order_relaxed);
    success.store(false, std::memory_order_relaxed);
    abort_flag.store(false, std::memory_order_relaxed);
    running.store(true, std::memory_order_release);

    th = std::thread([this, url, path, max_bytes] {
        run(url, path, max_bytes);
    });
}

bool NetMan::AsyncFetch::busy() const
{
    return th.joinable() && running.load(std::memory_order_acquire);
}

bool NetMan::AsyncFetch::poll()
{
    if (!th.joinable())
        return false;                           /* nothing (any more) */
    if (running.load(std::memory_order_acquire))
        return false;                           /* still downloading */
    th.join();                                  /* reap: result is safe */
    return true;
}

std::vector<uint8_t> NetMan::AsyncFetch::take_data()
{
    std::vector<uint8_t> out;
    out.swap(body);
    return out;
}

/* The download thread: everything it writes is either an atomic or,
 * for the payload, only touched by the caller after poll() joined. */
void NetMan::AsyncFetch::run(const std::string & url, const std::string & path,
                             size_t max_bytes)
{
    bool done_ok = false;
    std::string err;
    std::vector<uint8_t> payload;
    FILE * f = nullptr;

    HttpGet get;
    if (get.open(url, err)) {
        total_bytes.store(get.content_length(), std::memory_order_relaxed);

        if (!path.empty()) {
            f = fopen(path.c_str(), "wb");
            if (f == nullptr)
                err = "cannot write " + path;
        }

        if (f != nullptr || path.empty()) {
            done_ok = true;
            uint8_t chunk[CHUNK];
            for (;;) {
                if (abort_flag.load(std::memory_order_relaxed)) {
                    done_ok = false;
                    err = "cancelled";
                    break;
                }
                const int n = get.read(chunk, (int)sizeof(chunk));
                if (n < 0) {
                    done_ok = false;
                    err = "read failed";
                    break;
                }
                if (n == 0)
                    break;                      /* EOF */

                if (f != nullptr) {
                    if (fwrite(chunk, 1, (size_t)n, f) != (size_t)n) {
                        done_ok = false;
                        err = "write failed";
                        break;
                    }
                } else {
                    if (payload.size() + (size_t)n > max_bytes) {
                        done_ok = false;
                        err = "body too large";
                        break;
                    }
                    payload.insert(payload.end(), chunk, chunk + n);
                }

                got.store(got.load(std::memory_order_relaxed) + n,
                          std::memory_order_relaxed);
            }
        }
    }

    if (f != nullptr) {
        fclose(f);
        if (!done_ok)
            remove(path.c_str());               /* no partial files */
    }
    if (done_ok && f == nullptr)
        body.swap(payload);

    if (!done_ok)
        printf("NetMan: %s (%s)\n", err.c_str(), url.c_str());

    success.store(done_ok, std::memory_order_release);
    running.store(false, std::memory_order_release);
}
