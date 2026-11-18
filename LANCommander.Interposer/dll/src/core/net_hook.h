#pragma once

// Nesting depth of the socket hooks on the current thread. A hook that
// translates addresses (relay, IPX) only does so at depth 0, and holds a
// NetHookScope while it works, so the socket calls it makes on the game's
// behalf -- creating a portal, sending the translated datagram, the inner call
// of a wsock32 export that is implemented on top of ws2_32 -- pass straight
// through instead of being translated twice. The relay thread raises the depth
// for its whole life, so its own sockets are never translated at all.
//
// Defined by the network hooks (win/network.cpp, linux/sockets.cpp).
extern thread_local int t_netHookDepth;

struct NetHookScope
{
    NetHookScope()  { ++t_netHookDepth; }
    ~NetHookScope() { --t_netHookDepth; }
    NetHookScope(const NetHookScope&) = delete;
    NetHookScope& operator=(const NetHookScope&) = delete;
};
