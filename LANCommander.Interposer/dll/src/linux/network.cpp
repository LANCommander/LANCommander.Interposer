#include "network.h"
#include "config.h"
#include "fastdl.h"
#include "file_access.h"
#include "hooking.h"
#include "instance.h"
#include "network_filter.h"
#include "platform.h"

#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <sys/socket.h>

// The portable network pieces on Linux: DNS redirects (getaddrinfo,
// gethostbyname) and the adapter enumeration the launcher's picker reads. The
// relay, the IPX emulation and the active adapter-hiding filter are the socket
// hooks, which arrive with the relay.

namespace
{
    template <typename Fn>
    Fn Real(Fn& slot, const char* name)
    {
        if (!slot)
        {
            slot = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));

            if (!slot)
            {
                static void* libc = dlopen("libc.so.6", RTLD_LAZY | RTLD_NOLOAD);
                slot = libc ? reinterpret_cast<Fn>(dlsym(libc, name)) : nullptr;
            }
        }

        return slot;
    }

    // A DNS hook does nothing but call the trampoline before the hooks are on,
    // during the Interposer's own resolutions (FastDL), or with no rules.
    bool PassThrough()
    {
        return !hooking::Enabled() || InternalFileAccess::Active() || g_dnsRedirects.empty();
    }

    // The redirected form of `name`, or empty when no rule changed it. Logs the
    // redirect.
    std::string Redirect(const char* name)
    {
        if (!name || !name[0])
            return {};

        const std::wstring original   = platform::Utf8ToWide(name);
        const std::wstring redirected = ApplyDnsRedirect(original);

        if (redirected == original)
            return {};

        LogDnsRedirect(original.c_str(), redirected.c_str());
        return platform::WideToUtf8(redirected);
    }
}

// ── DNS redirects ─────────────────────────────────────────────────────────────

extern "C" INTERPOSER_API int getaddrinfo(const char* node, const char* service,
                                          const struct addrinfo* hints, struct addrinfo** res)
{
    static int (*real)(const char*, const char*, const struct addrinfo*, struct addrinfo**) = nullptr;
    Real(real, "getaddrinfo");

    if (PassThrough())
        return real(node, service, hints, res);

    if (std::string redirected = Redirect(node); !redirected.empty())
        return real(redirected.c_str(), service, hints, res);

    return real(node, service, hints, res);
}

extern "C" INTERPOSER_API struct hostent* gethostbyname(const char* name)
{
    static struct hostent* (*real)(const char*) = nullptr;
    Real(real, "gethostbyname");

    if (PassThrough())
        return real(name);

    if (std::string redirected = Redirect(name); !redirected.empty())
        return real(redirected.c_str());

    return real(name);
}

void InstallNetworkHooks()
{
    if (!g_dnsRedirects.empty())
    {
        hooking::LogHookStatus(L"libc", "getaddrinfo", hooking::Status::Ok);
        hooking::LogHookStatus(L"libc", "gethostbyname", hooking::Status::Ok);
    }

    if (netfilter::Active())
    {
        hooking::LogHookStatus(L"libc", "getifaddrs", hooking::Status::Ok);
        hooking::LogHookStatus(L"libc", "freeifaddrs", hooking::Status::Ok);
    }
}

// ── Adapter enumeration ───────────────────────────────────────────────────────

namespace
{
    // The real getifaddrs/freeifaddrs, for our own enumeration (which must see
    // every adapter) and for the filter hook to build on.
    int RealGetIfAddrs(ifaddrs** list)
    {
        static int (*real)(ifaddrs**) = nullptr;
        if (!real)
            real = reinterpret_cast<int (*)(ifaddrs**)>(dlsym(RTLD_NEXT, "getifaddrs"));
        return real ? real(list) : -1;
    }

    void RealFreeIfAddrs(ifaddrs* list)
    {
        static void (*real)(ifaddrs*) = nullptr;
        if (!real)
            real = reinterpret_cast<void (*)(ifaddrs*)>(dlsym(RTLD_NEXT, "freeifaddrs"));
        if (real)
            real(list);
    }

    void SetField(wchar_t* field, size_t capacity, const std::wstring& value)
    {
        const size_t count = value.size() < capacity - 1 ? value.size() : capacity - 1;
        std::memcpy(field, value.c_str(), count * sizeof(wchar_t));
        field[count] = L'\0';
    }

    void SetField(wchar_t* field, size_t capacity, const char* value)
    {
        SetField(field, capacity, platform::Utf8ToWide(value));
    }

    // What one interface accumulates across its getifaddrs entries.
    struct Adapter
    {
        InterposerNetworkAdapter info{};
        bool    subnetMatched = false;
        uint8_t mac[6]{};
        bool    hasMac = false;
    };
}

extern "C" INTERPOSER_API DWORD InterposerEnumNetworkAdapters(
    InterposerNetworkAdapter* buffer, DWORD capacity, DWORD* outCount)
{
    FORWARD_TO_PRIMARY(InterposerEnumNetworkAdapters, buffer, capacity, outCount);

    if (outCount)
        *outCount = 0;

    // The real call, not our filter hook: this reports the whole device set
    // with each adapter's allowed/hidden verdict, as the Windows export does.
    ifaddrs* list = nullptr;
    if (RealGetIfAddrs(&list) != 0)
        return 0;

    std::vector<Adapter>        adapters;
    std::map<std::string, size_t> byName;

    for (ifaddrs* ifa = list; ifa; ifa = ifa->ifa_next)
    {
        if (!ifa->ifa_name)
            continue;

        auto [it, inserted] = byName.try_emplace(ifa->ifa_name, adapters.size());

        if (inserted)
        {
            adapters.emplace_back();
            // Linux has no separate friendly name; the interface name is both.
            SetField(adapters.back().info.friendlyName, 256, ifa->ifa_name);
            SetField(adapters.back().info.description, 256, ifa->ifa_name);
        }

        Adapter& a = adapters[it->second];

        if (!ifa->ifa_addr)
            continue;

        const sa_family_t family = ifa->ifa_addr->sa_family;

        if (family == AF_INET)
        {
            const auto* in4 = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);

            if (a.info.ipv4Address[0] == L'\0')
            {
                char text[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &in4->sin_addr, text, sizeof(text));
                SetField(a.info.ipv4Address, 16, text);
            }

            if (netfilter::SubnetMatch(ntohl(in4->sin_addr.s_addr)))
                a.subnetMatched = true;
        }
        else if (family == AF_INET6)
        {
            if (a.info.ipv6Address[0] == L'\0')
            {
                const auto* in6 = reinterpret_cast<const sockaddr_in6*>(ifa->ifa_addr);
                char text[INET6_ADDRSTRLEN]{};
                inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof(text));
                SetField(a.info.ipv6Address, 46, text);
            }
        }
        else if (family == AF_PACKET)
        {
            const auto* ll = reinterpret_cast<const sockaddr_ll*>(ifa->ifa_addr);

            if (ll->sll_halen == 6 && !a.hasMac)
            {
                std::memcpy(a.mac, ll->sll_addr, 6);
                a.hasMac = true;

                wchar_t text[24]{};
                swprintf(text, 24, L"%02X:%02X:%02X:%02X:%02X:%02X",
                    a.mac[0], a.mac[1], a.mac[2], a.mac[3], a.mac[4], a.mac[5]);
                SetField(a.info.macAddress, 24, std::wstring(text));
            }
        }
    }

    RealFreeIfAddrs(list);

    const bool active = netfilter::Active();

    DWORD total = 0;
    for (Adapter& a : adapters)
    {
        const bool keep = a.subnetMatched ||
            (a.hasMac && netfilter::MacMatch(a.mac, 6)) ||
            netfilter::NameMatch(a.info.friendlyName);

        a.info.allowed = (active && !keep) ? 0 : 1;

        if (total < capacity && buffer)
            buffer[total] = a.info;

        ++total;
    }

    if (outCount)
        *outCount = (total < capacity) ? total : capacity;

    return total;
}

// ── Adapter hiding (the active filter) ────────────────────────────────────────
//
// getifaddrs returns one allocation that freeifaddrs frees through its head
// pointer, so we cannot free individual nodes. Instead we unlink every node of
// a hidden interface from the ifa_next chain -- hiding it from the caller's
// walk -- and remember, keyed by the head we return, the original head to free.
// Our freeifaddrs looks that up so the whole original block is still freed.

namespace
{
    std::mutex                      g_ifListMutex;
    std::map<ifaddrs*, ifaddrs*>    g_ifLists; // returned head -> original head

    // The interfaces to keep: those matching the filter, by name.
    std::map<std::string, bool> KeepByName(ifaddrs* list)
    {
        std::map<std::string, bool> keep;
        std::map<std::string, std::vector<uint8_t>> macs;

        for (ifaddrs* ifa = list; ifa; ifa = ifa->ifa_next)
        {
            if (!ifa->ifa_name)
                continue;

            const std::string name = ifa->ifa_name;
            bool& k = keep.try_emplace(name, false).first->second;

            if (netfilter::NameMatch(platform::Utf8ToWide(name)))
                k = true;

            if (!ifa->ifa_addr)
                continue;

            if (ifa->ifa_addr->sa_family == AF_INET)
            {
                const auto* in4 = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
                if (netfilter::SubnetMatch(ntohl(in4->sin_addr.s_addr)))
                    k = true;
            }
            else if (ifa->ifa_addr->sa_family == AF_PACKET)
            {
                const auto* ll = reinterpret_cast<const sockaddr_ll*>(ifa->ifa_addr);
                if (ll->sll_halen == 6 && netfilter::MacMatch(ll->sll_addr, 6))
                    k = true;
            }
        }

        return keep;
    }
}

extern "C" INTERPOSER_API int getifaddrs(ifaddrs** ifap)
{
    ifaddrs* list = nullptr;
    const int result = RealGetIfAddrs(&list);

    if (result != 0 || !ifap || !hooking::Enabled() || !netfilter::Active())
    {
        if (ifap)
            *ifap = list;
        else
            RealFreeIfAddrs(list);
        return result;
    }

    const std::map<std::string, bool> keep = KeepByName(list);

    // Unlink every node whose interface is not kept; the survivors keep their
    // relative order. The head may change, so track the original for freeing.
    ifaddrs* head = nullptr;
    ifaddrs* tail = nullptr;

    for (ifaddrs* ifa = list; ifa; ifa = ifa->ifa_next)
    {
        const auto it = ifa->ifa_name ? keep.find(ifa->ifa_name) : keep.end();
        const bool kept = it != keep.end() && it->second;

        if (!kept)
        {
            LogNetworkAccess(L"ADAPTER HIDE", platform::Utf8ToWide(ifa->ifa_name ? ifa->ifa_name : "").c_str());
            continue;
        }

        if (!head)
            head = ifa;
        else
            tail->ifa_next = ifa;

        tail = ifa;
    }

    if (tail)
        tail->ifa_next = nullptr;

    if (head)
    {
        std::lock_guard<std::mutex> lk(g_ifListMutex);
        g_ifLists[head] = list;
    }
    else
    {
        // Everything hidden: nothing to return, but the block still must be freed.
        RealFreeIfAddrs(list);
    }

    *ifap = head;
    return 0;
}

extern "C" INTERPOSER_API void freeifaddrs(ifaddrs* ifa)
{
    if (ifa)
    {
        std::lock_guard<std::mutex> lk(g_ifListMutex);
        if (auto it = g_ifLists.find(ifa); it != g_ifLists.end())
        {
            ifaddrs* original = it->second;
            g_ifLists.erase(it);
            RealFreeIfAddrs(original);
            return;
        }
    }

    RealFreeIfAddrs(ifa);
}

// The network hooks are what collect game-server addresses for FastDL to probe.
// Until the relay brings the socket hooks nothing is collected, so there is
// nothing to probe and FastDL works from a configured BaseUrl alone.
void ProbeAllDiscoveredAddresses()
{
}
