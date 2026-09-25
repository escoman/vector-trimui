#pragma once

#include <atomic>
#include <inttypes.h>
#include <stddef.h>
#include <string>
#include <thread>
#include <vector>

/*
 * Network access for the Game Center, TrimUI edition.
 *
 * On the PSP this module owned the whole WiFi lifecycle (net modules,
 * the system AP dialog, sceHttp). On the Brick Pro the link is owned
 * by CrossMix/the OS, so nothing has to be initialised or connected
 * here: what is left is plain HTTP/1.0 over BSD sockets (the ROM
 * server has no TLS, so no libcurl/OpenSSL dependency) and a small
 * background fetcher.
 *
 * The fetcher exists because the UI runs on the render thread at
 * ~50 Hz: a blocking GET there would freeze the menu and the whole
 * overlay for the duration of the download. AsyncFetch therefore does
 * the work in its own thread and the window only polls the progress.
 *
 * TRIMUI-only: the desktop Makefile globs src/ and never sees gui/,
 * so the POSIX socket code stays out of the desktop/Windows builds.
 */
namespace NetMan {

/* One blocking HTTP GET, following redirects. Used by AsyncFetch and
 * handy on its own for tools; never call it from the UI thread.
 * Returns true and fills `out` with the response body (capped at
 * max_bytes), false on any error with a reason in `err`. */
bool http_get(const std::string & url, std::vector<uint8_t> & out,
              size_t max_bytes, std::string & err);

/* Background download with progress, one job at a time:
 *
 *   start_memory(url, max_bytes) - the body ends up in take_data()
 *   start_file(url, path)        - the body is streamed into a file
 *                                  (removed again when it fails)
 *   busy()                       - a job is still running
 *   poll()                       - the job has just finished; it also
 *                                  reaps the thread, so the result and
 *                                  the payload are safe to read. Only
 *                                  the first poll() after the end
 *                                  returns true.
 *   ok()                         - result of the finished job
 *   received()/total()           - progress; total() is -1 when the
 *                                  server sent no Content-Length
 *
 * start_*() on a running job and the destructor abort it first: the
 * thread checks the abort flag between reads and the socket carries a
 * receive timeout, so the wait stays short.
 */
class AsyncFetch
{
public:
    AsyncFetch();
    ~AsyncFetch();

    AsyncFetch(const AsyncFetch &) = delete;
    AsyncFetch & operator=(const AsyncFetch &) = delete;

    void start_memory(const std::string & url, size_t max_bytes);
    void start_file(const std::string & url, const std::string & path);

    bool busy() const;
    bool poll();
    bool ok() const { return success.load(std::memory_order_acquire); }
    long long received() const { return got.load(std::memory_order_relaxed); }
    long long total() const { return total_bytes.load(std::memory_order_relaxed); }

    /* Moves the downloaded body out; only valid after poll() returned
     * true for a memory job (the thread has been reaped by then). */
    std::vector<uint8_t> take_data();

    /* Abort a running job and reap its thread. Safe when idle. */
    void cancel();

private:
    void begin(const std::string & url, const std::string & path,
               size_t max_bytes);
    void run(const std::string & url, const std::string & path,
             size_t max_bytes);

    std::thread th;
    std::atomic<bool> running;
    std::atomic<bool> abort_flag;
    std::atomic<bool> success;
    std::atomic<long long> got;
    std::atomic<long long> total_bytes;
    std::vector<uint8_t> body;   /* written by the thread, read after join */
};

} /* namespace NetMan */
