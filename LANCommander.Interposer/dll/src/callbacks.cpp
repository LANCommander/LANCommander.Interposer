#include "callbacks.h"
#include <atomic>

// ---------------------------------------------------------------------------
// Atomic callback storage — one pointer per category, initially null.
// Using relaxed ordering: registration is rare and happens before hooks fire,
// so we don't need acquire/release semantics on every load.
// ---------------------------------------------------------------------------
static std::atomic<InterposerFileCallback>     g_fileCallback{nullptr};
static std::atomic<InterposerRegistryCallback> g_registryCallback{nullptr};
static std::atomic<InterposerDnsCallback>      g_dnsCallback{nullptr};
static std::atomic<InterposerNetworkCallback>  g_networkCallback{nullptr};
static std::atomic<InterposerIdentityCallback> g_identityCallback{nullptr};

// ---------------------------------------------------------------------------
// Registration exports
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) void InterposerRegisterFileCallback(InterposerFileCallback cb)
{
    g_fileCallback.store(cb, std::memory_order_release);
}

extern "C" __declspec(dllexport) void InterposerRegisterRegistryCallback(InterposerRegistryCallback cb)
{
    g_registryCallback.store(cb, std::memory_order_release);
}

extern "C" __declspec(dllexport) void InterposerRegisterDnsCallback(InterposerDnsCallback cb)
{
    g_dnsCallback.store(cb, std::memory_order_release);
}

extern "C" __declspec(dllexport) void InterposerRegisterNetworkCallback(InterposerNetworkCallback cb)
{
    g_networkCallback.store(cb, std::memory_order_release);
}

extern "C" __declspec(dllexport) void InterposerRegisterIdentityCallback(InterposerIdentityCallback cb)
{
    g_identityCallback.store(cb, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Fire helpers
// ---------------------------------------------------------------------------
void FireFileCallback(const wchar_t* verb, const wchar_t* path, const wchar_t* secondaryPath)
{
    auto cb = g_fileCallback.load(std::memory_order_acquire);
    if (cb) cb(verb, path, secondaryPath);
}

void FireRegistryCallback(const wchar_t* verb, const wchar_t* keyPath, const wchar_t* valueName)
{
    auto cb = g_registryCallback.load(std::memory_order_acquire);
    if (cb) cb(verb, keyPath, valueName);
}

void FireDnsCallback(const wchar_t* hostname, const wchar_t* redirectedHostname)
{
    auto cb = g_dnsCallback.load(std::memory_order_acquire);
    if (cb) cb(hostname, redirectedHostname);
}

void FireNetworkCallback(const wchar_t* address, int port)
{
    auto cb = g_networkCallback.load(std::memory_order_acquire);
    if (cb) cb(address, port);
}

void FireIdentityCallback(const wchar_t* type, const wchar_t* value)
{
    auto cb = g_identityCallback.load(std::memory_order_acquire);
    if (cb) cb(type, value);
}
