#include "net_hook.h"
#include "hooking.h"
#include "relay.h"

#include <cerrno>
#include <cstring>
#include <vector>

#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// The socket interposers: the relay's address translation, the Linux side of
// what win/network.cpp's Winsock hooks do. Preloaded, these definitions come
// first in symbol resolution, so the game's calls land here; each forwards to
// libc's with dlsym(RTLD_NEXT). They only translate addresses -- every game
// socket stays a real socket (see core/relay.cpp) -- and they pass straight
// through unless the relay wanted its hooks at startup (InitRelay), and at
// depth > 0 (net_hook.h), which is how the relay thread's own sockets and the
// calls a hook makes on the game's behalf are left alone.
//
// One translation the Winsock side has no need for: Linux hands out the lowest
// free descriptor, so a number is reused the moment it is closed. close() makes
// the relay forget the descriptor, and socket() and accept() forget it again
// for one closed some way the hooks never saw (dup2 over it, fclose of an
// fdopen), so a new socket never inherits an old one's virtual peer.

thread_local int t_netHookDepth = 0;

namespace
{
    template <typename Fn>
    Fn Real(Fn& slot, const char* name)
    {
        if (!slot)
        {
            slot = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));

            // A dlopen'ed copy (rather than a preloaded one) has nothing next.
            if (!slot)
            {
                static void* libc = dlopen("libc.so.6", RTLD_LAZY | RTLD_NOLOAD);
                slot = libc ? reinterpret_cast<Fn>(dlsym(libc, name)) : nullptr;
            }
        }

        return slot;
    }

    bool Translating()
    {
        return t_netHookDepth == 0 && RelayHooksWanted() && hooking::Enabled();
    }

    // Forget whatever the relay knew about a descriptor number.
    void Forget(int fd)
    {
        if (fd >= 0 && t_netHookDepth == 0 && RelayHooksWanted())
            RelayOnSocketClosed(fd);
    }

    // SOCK_STREAM or SOCK_DGRAM; 0 when the descriptor cannot say.
    int SocketType(int fd)
    {
        int       type = 0;
        socklen_t len  = sizeof(type);
        return getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0 ? type : 0;
    }

    // Where a send to `to` should really go: its portal, or the relay's
    // gateway. False to send as the game asked.
    bool MapDestination(int fd, const sockaddr* to, socklen_t tolen, sockaddr_in& mapped, bool connecting)
    {
        if (!to)
            return false;

        if (connecting && SocketType(fd) == SOCK_STREAM)
            return RelayPrepareStreamConnect(fd, to, static_cast<int>(tolen), mapped);

        return RelayMapDatagramDestination(to, static_cast<int>(tolen), mapped);
    }

    // A datagram the relay delivered arrives from a portal on loopback; the
    // game is shown the virtual endpoint it stands for.
    void MapSource(sockaddr* from, socklen_t fromlen)
    {
        if (from && fromlen)
            RelayMapDatagramSource(from, static_cast<int>(fromlen));
    }

    // An accepted relay stream comes from the relay's connector on loopback;
    // the game is shown the remote virtual endpoint. Done even when the game
    // passed no address buffer, so getpeername can answer later.
    void MapAccepted(int fd, sockaddr* addr, socklen_t* addrlen)
    {
        if (fd < 0 || !RelayActive())
            return;

        if (addr && addrlen)
        {
            RelayMapStreamPeer(fd, addr, static_cast<int>(*addrlen));
            return;
        }

        static int (*realGetPeerName)(int, sockaddr*, socklen_t*) = nullptr;
        Real(realGetPeerName, "getpeername");

        sockaddr_in peer{};
        socklen_t   peerLen = sizeof(peer);
        if (realGetPeerName(fd, reinterpret_cast<sockaddr*>(&peer), &peerLen) == 0)
            RelayMapStreamPeer(fd, reinterpret_cast<sockaddr*>(&peer), static_cast<int>(peerLen));
    }

    // Keep errno across the bookkeeping that follows a real call.
    struct SaveErrno
    {
        int saved = errno;
        ~SaveErrno() { errno = saved; }
    };
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

extern "C" INTERPOSER_API int socket(int domain, int type, int protocol)
{
    static int (*real)(int, int, int) = nullptr;
    Real(real, "socket");

    const int fd = real(domain, type, protocol);
    SaveErrno keep;
    Forget(fd);
    return fd;
}

extern "C" INTERPOSER_API int close(int fd)
{
    static int (*real)(int) = nullptr;
    Real(real, "close");

    // Before the descriptor is released: once it is, another thread can be
    // handed the same number for a brand-new socket.
    Forget(fd);
    return real(fd);
}

extern "C" INTERPOSER_API int bind(int fd, const sockaddr* addr, socklen_t len)
{
    static int (*real)(int, const sockaddr*, socklen_t) = nullptr;
    Real(real, "bind");

    if (!Translating())
        return real(fd, addr, len);

    NetHookScope scope;
    sockaddr_in  mapped{};

    if (RelayMapBind(fd, addr, static_cast<int>(len), mapped))
        return real(fd, reinterpret_cast<const sockaddr*>(&mapped), sizeof(mapped));

    return real(fd, addr, len);
}

extern "C" INTERPOSER_API int connect(int fd, const sockaddr* addr, socklen_t len)
{
    static int (*real)(int, const sockaddr*, socklen_t) = nullptr;
    Real(real, "connect");

    if (!Translating())
        return real(fd, addr, len);

    NetHookScope scope;
    sockaddr_in  mapped{};

    if (MapDestination(fd, addr, len, mapped, true))
        return real(fd, reinterpret_cast<const sockaddr*>(&mapped), sizeof(mapped));

    return real(fd, addr, len);
}

extern "C" INTERPOSER_API int accept(int fd, sockaddr* addr, socklen_t* addrlen)
{
    static int (*real)(int, sockaddr*, socklen_t*) = nullptr;
    Real(real, "accept");

    const int accepted = real(fd, addr, addrlen);
    SaveErrno keep;
    Forget(accepted);

    if (Translating())
    {
        NetHookScope scope;
        MapAccepted(accepted, addr, addrlen);
    }

    return accepted;
}

extern "C" INTERPOSER_API int accept4(int fd, sockaddr* addr, socklen_t* addrlen, int flags)
{
    static int (*real)(int, sockaddr*, socklen_t*, int) = nullptr;
    Real(real, "accept4");

    const int accepted = real(fd, addr, addrlen, flags);
    SaveErrno keep;
    Forget(accepted);

    if (Translating())
    {
        NetHookScope scope;
        MapAccepted(accepted, addr, addrlen);
    }

    return accepted;
}

// ── Names ─────────────────────────────────────────────────────────────────────

extern "C" INTERPOSER_API int getsockname(int fd, sockaddr* addr, socklen_t* len)
{
    static int (*real)(int, sockaddr*, socklen_t*) = nullptr;
    Real(real, "getsockname");

    const int result = real(fd, addr, len);

    if (result == 0 && len && Translating())
    {
        NetHookScope scope;
        RelayMapLocalName(fd, addr, static_cast<int>(*len));
    }

    return result;
}

extern "C" INTERPOSER_API int getpeername(int fd, sockaddr* addr, socklen_t* len)
{
    static int (*real)(int, sockaddr*, socklen_t*) = nullptr;
    Real(real, "getpeername");

    const int result = real(fd, addr, len);

    if (result == 0 && len && Translating())
    {
        NetHookScope scope;

        if (SocketType(fd) == SOCK_STREAM)
            RelayMapStreamPeer(fd, addr, static_cast<int>(*len));
        else
            MapSource(addr, *len);
    }

    return result;
}

// ── Sending ───────────────────────────────────────────────────────────────────

extern "C" INTERPOSER_API ssize_t sendto(int fd, const void* buf, size_t n, int flags,
                                         const sockaddr* to, socklen_t tolen)
{
    static ssize_t (*real)(int, const void*, size_t, int, const sockaddr*, socklen_t) = nullptr;
    Real(real, "sendto");

    if (!to || !Translating())
        return real(fd, buf, n, flags, to, tolen);

    NetHookScope scope;
    sockaddr_in  mapped{};

    if (MapDestination(fd, to, tolen, mapped, false))
        return real(fd, buf, n, flags, reinterpret_cast<const sockaddr*>(&mapped), sizeof(mapped));

    return real(fd, buf, n, flags, to, tolen);
}

// ENet, and with it a good many games, sends and receives this way.
extern "C" INTERPOSER_API ssize_t sendmsg(int fd, const msghdr* msg, int flags)
{
    static ssize_t (*real)(int, const msghdr*, int) = nullptr;
    Real(real, "sendmsg");

    if (!msg || !msg->msg_name || !Translating())
        return real(fd, msg, flags);

    NetHookScope scope;
    sockaddr_in  mapped{};

    if (!MapDestination(fd, static_cast<const sockaddr*>(msg->msg_name), msg->msg_namelen, mapped, false))
        return real(fd, msg, flags);

    msghdr copy     = *msg;
    copy.msg_name    = &mapped;
    copy.msg_namelen = sizeof(mapped);
    return real(fd, &copy, flags);
}

extern "C" INTERPOSER_API int sendmmsg(int fd, mmsghdr* vec, unsigned int vlen, int flags)
{
    static int (*real)(int, mmsghdr*, unsigned int, int) = nullptr;
    Real(real, "sendmmsg");

    if (!vec || !vlen || !Translating())
        return real(fd, vec, vlen, flags);

    NetHookScope scope;

    std::vector<mmsghdr>     copy(vec, vec + vlen);
    std::vector<sockaddr_in> mapped(vlen);
    bool                     any = false;

    for (unsigned int i = 0; i < vlen; ++i)
    {
        const msghdr& h = vec[i].msg_hdr;

        if (h.msg_name &&
            MapDestination(fd, static_cast<const sockaddr*>(h.msg_name), h.msg_namelen, mapped[i], false))
        {
            copy[i].msg_hdr.msg_name    = &mapped[i];
            copy[i].msg_hdr.msg_namelen = sizeof(sockaddr_in);
            any = true;
        }
    }

    if (!any)
        return real(fd, vec, vlen, flags);

    const int sent = real(fd, copy.data(), vlen, flags);

    for (int i = 0; i < sent; ++i)
        vec[i].msg_len = copy[i].msg_len;

    return sent;
}

// ── Receiving ─────────────────────────────────────────────────────────────────

extern "C" INTERPOSER_API ssize_t recvfrom(int fd, void* buf, size_t n, int flags,
                                           sockaddr* from, socklen_t* fromlen)
{
    static ssize_t (*real)(int, void*, size_t, int, sockaddr*, socklen_t*) = nullptr;
    Real(real, "recvfrom");

    const ssize_t result = real(fd, buf, n, flags, from, fromlen);

    if (result >= 0 && from && fromlen && Translating())
    {
        SaveErrno    keep;
        NetHookScope scope;
        MapSource(from, *fromlen);
    }

    return result;
}

extern "C" INTERPOSER_API ssize_t recvmsg(int fd, msghdr* msg, int flags)
{
    static ssize_t (*real)(int, msghdr*, int) = nullptr;
    Real(real, "recvmsg");

    const ssize_t result = real(fd, msg, flags);

    if (result >= 0 && msg && msg->msg_name && Translating())
    {
        SaveErrno    keep;
        NetHookScope scope;
        MapSource(static_cast<sockaddr*>(msg->msg_name), msg->msg_namelen);
    }

    return result;
}

extern "C" INTERPOSER_API int recvmmsg(int fd, mmsghdr* vec, unsigned int vlen, int flags, timespec* timeout)
{
    static int (*real)(int, mmsghdr*, unsigned int, int, timespec*) = nullptr;
    Real(real, "recvmmsg");

    const int received = real(fd, vec, vlen, flags, timeout);

    if (received > 0 && vec && Translating())
    {
        SaveErrno    keep;
        NetHookScope scope;

        for (int i = 0; i < received; ++i)
            MapSource(static_cast<sockaddr*>(vec[i].msg_hdr.msg_name), vec[i].msg_hdr.msg_namelen);
    }

    return received;
}
