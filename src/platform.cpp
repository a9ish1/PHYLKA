#include "platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <bcrypt.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#endif
#if defined(__linux__)
#include <sys/random.h>
#include <termios.h>
#elif !defined(_WIN32)
#include <readpassphrase.h>
#endif

namespace plk::platform {

static void secure_zero(void* p, size_t n) {
    volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
    while (n--) *v++ = 0;
}

void random_bytes(uint8_t* out, size_t len) {
#if defined(_WIN32)
    if (len && BCryptGenRandom(nullptr, out, ULONG(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) std::abort();
#elif defined(__linux__)
    while (len) {
        ssize_t r = getrandom(out, len, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            std::abort();
        }
        out += r;
        len -= size_t(r);
    }
#else
    arc4random_buf(out, len);
#endif
}

#if defined(_WIN32)

bool has_tty() {
    DWORD m;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &m) != 0;
}

std::string read_secret(const char* prompt, bool echo) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (!GetConsoleMode(in, &mode)) return "";
    std::fputs(prompt, stderr);
    std::fflush(stderr);
    if (!echo) SetConsoleMode(in, mode & ~DWORD(ENABLE_ECHO_INPUT));
    char buf[1024];
    bool ok = std::fgets(buf, sizeof buf, stdin) != nullptr;
    SetConsoleMode(in, mode);
    if (!echo) std::fputs("\n", stderr);
    std::string s;
    if (ok) {
        size_t n = std::strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
        s.assign(buf, n);
    }
    secure_zero(buf, sizeof buf);
    return s;
}

#elif defined(__linux__)

bool has_tty() {
    int fd = ::open("/dev/tty", O_RDWR | O_NOCTTY);
    if (fd < 0) return false;
    ::close(fd);
    return true;
}

std::string read_secret(const char* prompt, bool echo) {
    int fd = ::open("/dev/tty", O_RDWR | O_NOCTTY);
    if (fd < 0) return "";
    termios old{}, noecho{};
    bool have = tcgetattr(fd, &old) == 0;
    if (have && !echo) {
        noecho = old;
        noecho.c_lflag &= ~tcflag_t(ECHO);
        tcsetattr(fd, TCSAFLUSH, &noecho);
    }
    size_t pl = std::strlen(prompt);
    if (::write(fd, prompt, pl) < 0) {}
    char buf[1024];
    size_t n = 0;
    bool ok = true;
    while (n < sizeof buf - 1) {
        char c;
        ssize_t r = ::read(fd, &c, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { ok = n > 0; break; }
        if (c == '\n' || c == '\r') break;
        buf[n++] = c;
    }
    if (have && !echo) {
        tcsetattr(fd, TCSAFLUSH, &old);
        if (::write(fd, "\n", 1) < 0) {}
    }
    ::close(fd);
    std::string s = ok ? std::string(buf, n) : std::string();
    secure_zero(buf, sizeof buf);
    return s;
}

#else  // Apple / BSD

bool has_tty() {
    int fd = ::open("/dev/tty", O_RDWR | O_NOCTTY);
    if (fd < 0) return false;
    ::close(fd);
    return true;
}

std::string read_secret(const char* prompt, bool echo) {
    char buf[1024];
    if (!readpassphrase(prompt, buf, sizeof buf, (echo ? RPP_ECHO_ON : RPP_ECHO_OFF) | RPP_REQUIRE_TTY)) return "";
    std::string s(buf);
    secure_zero(buf, sizeof buf);
    return s;
}

#endif

bool net_init() {
#ifdef _WIN32
    static bool done = false, ok = false;
    if (!done) {
        WSADATA d;
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
        done = true;
    }
    return ok;
#else
    return true;
#endif
}

void close_socket(socket_t fd) {
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

void set_nosigpipe(socket_t fd) {
#ifdef __APPLE__
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd;
#endif
}

bool set_recv_timeout(socket_t fd, int seconds) {
#ifdef _WIN32
    DWORD ms = DWORD(seconds) * 1000;
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms) == 0;
#else
    timeval tv{};
    tv.tv_sec = seconds;
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0;
#endif
}

}  // namespace plk::platform
