// PHYLKA - thin OS abstraction (randomness, secret input, sockets) for macOS, Linux and Windows.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

namespace plk::platform {

#ifdef _WIN32
using socket_t = SOCKET;
inline const socket_t INVALID_SOCK = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t INVALID_SOCK = -1;
#endif

/// Fills the buffer with OS randomness; aborts the process if the OS source fails.
void random_bytes(uint8_t* out, size_t len);

/// Reads a line from the terminal (not stdin) without echo (echo=false) or with it. "" on failure / no tty.
std::string read_secret(const char* prompt, bool echo);
/// True if an interactive terminal is available for read_secret
bool has_tty();

/// Call once at startup before any socket use (WSAStartup on Windows, no-op elsewhere). False on failure.
bool net_init();
/// Closes a socket (closesocket on Windows, close elsewhere)
void close_socket(socket_t fd);
/// Suppress SIGPIPE on this socket where the OS needs it (SO_NOSIGPIPE on Apple)
void set_nosigpipe(socket_t fd);
/// Receive timeout in seconds; false on failure
bool set_recv_timeout(socket_t fd, int seconds);

/// Pass as the flags argument of send(): MSG_NOSIGNAL on Linux, 0 elsewhere
#if defined(__linux__)
inline constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
inline constexpr int SEND_FLAGS = 0;
#endif

}  // namespace plk::platform
