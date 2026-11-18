#pragma once

// BSD sockets, as Winsock and as POSIX: the little that differs between the two
// for code shared across platforms (the network relay). Header-only, because
// every function is a line or two, and because on Windows the translation unit
// may have to define FD_SETSIZE before winsock2.h is first included.

#include <cstdint>
#include <cstring>

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

using SockLen = int;

namespace sock
{
    // send() flags for a stream: Winsock raises no signal for a closed peer.
    constexpr int kSendFlags = 0;

    inline int  Close(SOCKET s)         { return closesocket(s); }
    inline void SetNonBlocking(SOCKET s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }

    // The last call failed only because it would have blocked.
    inline bool WouldBlock()
    {
        const int e = WSAGetLastError();
        return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINTR;
    }
}

#else

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <unordered_map>
#include <vector>

using SOCKET  = int;
using SockLen = socklen_t;

#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#ifndef SOCKET_ERROR
#define SOCKET_ERROR (-1)
#endif
#ifndef SD_SEND
#define SD_SEND SHUT_WR
#endif

namespace sock
{
    // A peer that has gone away would otherwise raise SIGPIPE, which ends the
    // game unless it ignores the signal itself.
    constexpr int kSendFlags = MSG_NOSIGNAL;

    inline int Close(SOCKET s) { return close(s); }

    inline void SetNonBlocking(SOCKET s)
    {
        const int flags = fcntl(s, F_GETFL, 0);
        if (flags >= 0)
            fcntl(s, F_SETFL, flags | O_NONBLOCK);
    }

    inline bool WouldBlock()
    {
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR;
    }
}

#endif

namespace sock
{
    // The sockets one wait covers, and what each is waiting for. select() on
    // Windows, where FD_SETSIZE is the caller's to raise and WSAPoll is not on
    // XP; poll() elsewhere, where select() cannot take a descriptor numbered
    // FD_SETSIZE (1024) or above, and a game may well hold that many.
    class PollSet
    {
    public:
        void Add(SOCKET s, bool read, bool write, bool except = false);

        // Wait up to timeoutMs. The number of ready sockets, 0 on timeout, or
        // negative on error.
        int Wait(int timeoutMs);

        bool Readable(SOCKET s) const;
        bool Writable(SOCKET s) const;
        bool Failed(SOCKET s) const;   // an exceptional condition: a failed connect

    private:
#ifdef _WIN32
        fd_set m_read{}, m_write{}, m_except{};
        bool   m_any = false;
#else
        std::vector<pollfd>             m_fds;
        std::unordered_map<int, size_t> m_index;
        short Events(SOCKET s) const;
#endif
    };

#ifdef _WIN32

    inline void PollSet::Add(SOCKET s, bool read, bool write, bool except)
    {
        if (!m_any)
        {
            FD_ZERO(&m_read); FD_ZERO(&m_write); FD_ZERO(&m_except);
            m_any = true;
        }

        if (read)   FD_SET(s, &m_read);
        if (write)  FD_SET(s, &m_write);
        if (except) FD_SET(s, &m_except);
    }

    inline int PollSet::Wait(int timeoutMs)
    {
        if (!m_any)
        {
            Sleep(static_cast<DWORD>(timeoutMs));
            return 0;
        }

        timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
        return select(0, &m_read, &m_write, &m_except, &tv);
    }

    inline bool PollSet::Readable(SOCKET s) const { return m_any && FD_ISSET(s, &m_read); }
    inline bool PollSet::Writable(SOCKET s) const { return m_any && FD_ISSET(s, &m_write); }
    inline bool PollSet::Failed(SOCKET s) const   { return m_any && FD_ISSET(s, &m_except); }

#else

    inline void PollSet::Add(SOCKET s, bool read, bool write, bool except)
    {
        // POLLERR and POLLHUP are always reported; there is nothing to ask for
        // to see a failed connect.
        (void)except;

        auto [it, inserted] = m_index.try_emplace(s, m_fds.size());
        if (inserted)
            m_fds.push_back(pollfd{ s, 0, 0 });

        pollfd& p = m_fds[it->second];
        if (read)  p.events |= POLLIN;
        if (write) p.events |= POLLOUT;
    }

    inline int PollSet::Wait(int timeoutMs)
    {
        return poll(m_fds.data(), m_fds.size(), timeoutMs);
    }

    inline short PollSet::Events(SOCKET s) const
    {
        const auto it = m_index.find(s);
        return it == m_index.end() ? 0 : m_fds[it->second].revents;
    }

    // An error or a hang-up reads as readable, so the read that follows
    // discovers it, as select() reports it on Windows.
    inline bool PollSet::Readable(SOCKET s) const { return Events(s) & (POLLIN | POLLERR | POLLHUP); }
    inline bool PollSet::Writable(SOCKET s) const { return Events(s) & POLLOUT; }
    inline bool PollSet::Failed(SOCKET s) const   { return Events(s) & POLLERR; }

#endif
}
