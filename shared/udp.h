// ============================================================
//  VoiceChat — Minimal cross-platform UDP socket
//  File: shared/udp.h
//
//  One place for the winsock/posix #ifdef dance (previously copied into
//  the plugin, the relay and the ASI). Header-only, no dependencies.
// ============================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using vc_socklen_t = int;
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
   using vc_socklen_t = socklen_t;
#endif

namespace vc {

inline void netStartup()
{
#ifdef _WIN32
    static bool done = false;
    if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = true; }
#endif
}

class UdpSocket {
public:
#ifdef _WIN32
    using Handle = SOCKET;
    static constexpr Handle kInvalid = INVALID_SOCKET;
#else
    using Handle = int;
    static constexpr Handle kInvalid = -1;
#endif

    UdpSocket() = default;
    ~UdpSocket() { close(); }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool open()
    {
        netStartup();
        m_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        return m_fd != kInvalid;
    }

    bool bindAny(uint16_t port)
    {
        sockaddr_in a{}; a.sin_family = AF_INET;
        a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(port);
        return ::bind(m_fd, (sockaddr*)&a, sizeof(a)) == 0;
    }

    // Receive timeout so loops can poll a shutdown flag.
    void setRecvTimeout(int ms)
    {
#ifdef _WIN32
        DWORD t = (DWORD)ms;
        setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, (char*)&t, sizeof(t));
#else
        timeval t{ ms / 1000, (ms % 1000) * 1000 };
        setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
#endif
    }

    static sockaddr_in addr(const char* ip, uint16_t port)
    {
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
        inet_pton(AF_INET, ip, &a.sin_addr);
        return a;
    }

    int sendTo(const void* buf, int len, const sockaddr_in& to)
    {
        return ::sendto(m_fd, (const char*)buf, len, 0,
                        (const sockaddr*)&to, sizeof(to));
    }

    int recvFrom(void* buf, int cap, sockaddr_in& from)
    {
        vc_socklen_t fl = sizeof(from);
        return ::recvfrom(m_fd, (char*)buf, cap, 0, (sockaddr*)&from, &fl);
    }

    void close()
    {
        if (m_fd != kInvalid) {
#ifdef _WIN32
            closesocket(m_fd);
#else
            ::close(m_fd);
#endif
            m_fd = kInvalid;
        }
    }

    bool   valid() const { return m_fd != kInvalid; }
    Handle handle() const { return m_fd; }

private:
    Handle m_fd = kInvalid;
};

} // namespace vc
