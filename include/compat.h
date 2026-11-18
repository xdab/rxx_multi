#ifndef COMPAT_H
#define COMPAT_H

/**
 * @file compat.h
 * @brief Windows shims for the POSIX surface the core uses (sockets, binary
 *        stdio); every shim is a no-op or its POSIX function on Linux.
 *
 * On Windows sockets live in Winsock (different error codes, @c closesocket,
 * @c WSAStartup) and the pre-opened std handles default to text mode: 0x0A
 * expands to CRLF on write and 0x1A ends reads, corrupting any PCM/sample
 * stream crossing stdout — callers opt into binary mode via
 * @ref stdio_binary.
 */

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <errno.h> // IWYU pragma: export
#include <fcntl.h> // IWYU pragma: export
#include <io.h>    // IWYU pragma: export
#include <stdint.h>
#include <stdio.h>
#include <winsock2.h> // IWYU pragma: export
#include <ws2tcpip.h> // IWYU pragma: export

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* Winsock never raises SIGPIPE */
#endif

/* WSA error codes mapped onto the errno names the core checks for */
static inline int net_errno(void)
{
    switch (WSAGetLastError())
    {
    case WSAEINTR:
        return EINTR;
    case WSAEWOULDBLOCK:
        return EAGAIN;
    case WSAECONNRESET:
        return ECONNRESET;
    case WSAECONNABORTED:
    case WSAESHUTDOWN:
        return EPIPE;
    default:
        return errno;
    }
}

static inline int net_init(void)
{
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
}

static inline void net_cleanup(void) { WSACleanup(); }

/* Winsock sockets are not file descriptors: close() is closesocket() */
static inline int net_close(int fd) { return closesocket((SOCKET)fd); }

/* Winsock's option value is const char * with an int length */
static inline int
net_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen)
{
    return setsockopt((SOCKET)fd, level, optname, (const char *)optval, (int)optlen);
}

/* Winsock send()/sendto() take int lengths and return int */
static inline ssize_t net_send(int fd, const void *buf, size_t len, int flags)
{
    return (ssize_t)send((SOCKET)fd, (const char *)buf, (int)len, flags);
}

static inline ssize_t net_sendto(
    int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest, socklen_t destlen)
{
    return (ssize_t)sendto((SOCKET)fd, (const char *)buf, (int)len, flags, dest, (int)destlen);
}

static inline void stdio_binary(void)
{
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
}

#else /* POSIX */

#include <arpa/inet.h>   // IWYU pragma: export
#include <errno.h>       // IWYU pragma: export
#include <netinet/in.h>  // IWYU pragma: export
#include <netinet/tcp.h> // IWYU pragma: export
#include <stdio.h>
#include <sys/select.h> // IWYU pragma: export
#include <sys/socket.h> // IWYU pragma: export
#include <sys/types.h>  // IWYU pragma: export
#include <unistd.h>     // IWYU pragma: export

static inline int net_errno(void) { return errno; }
static inline int net_init(void) { return 0; }
static inline void net_cleanup(void) {}
static inline int net_close(int fd) { return close(fd); }
static inline int
net_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen)
{
    return setsockopt(fd, level, optname, optval, optlen);
}
static inline ssize_t net_send(int fd, const void *buf, size_t len, int flags)
{
    return send(fd, buf, len, flags);
}
static inline ssize_t net_sendto(
    int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest, socklen_t destlen)
{
    return sendto(fd, buf, len, flags, dest, destlen);
}
static inline void stdio_binary(void) {}

#endif /* _WIN32 */

#endif /* COMPAT_H */
