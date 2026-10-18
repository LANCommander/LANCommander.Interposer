#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <MinHook.h>
#include <algorithm>
#include <cwctype>
#include <mutex>
#include <string>
#include <vector>

#include "audio.h"
#include "config.h"

// ---------------------------------------------------------------------------
// Audio device filtering.
//
// `AudioDevices` is a per-direction allow-list: `Output` and `Input` each hold
// entries, and an entry is matched as a case-insensitive substring against
// every string the device is known by — friendly name, description, Core Audio
// endpoint ID, device instance path, DirectSound GUID, and the hardware IDs of
// the physical device behind the endpoint. One entry can therefore name a
// device however the user happens to have it written down.
//
// That only works because the three APIs a game can enumerate through report
// wildly different amounts about the same device, so everything is resolved
// back to one **identity table** built once from Core Audio:
//
//   winmm        Reports a name truncated to 31 characters and no identifier at
//                all. `waveOutMessage(DRV_QUERYDEVICEINTERFACE)` yields the
//                endpoint's device interface path, which carries the Core Audio
//                endpoint ID — the one link winmm offers to the rest of the
//                stack. That looks the device up in the table and it inherits
//                the full identity. Device indices are an index space, so
//                hiding one renumbers the rest.
//
//   DirectSound  Reports a description and a device GUID. The GUID is the
//                endpoint's `PKEY_AudioEndpoint_GUID`, so it looks up in the
//                table too; the description is the fallback.
//
//   Core Audio   Reports everything directly, so it needs no table at all.
//
// Hardware IDs come from walking the device tree upwards with cfgmgr32, which
// is what turns `USB\VID_0965&PID_9001` into a usable rule: the endpoint's own
// instance path says nothing about the hardware, but its parents do.
//
// Nothing is hooked unless the filter is active or Logging.Audio is set, so
// leaving the section at its defaults costs the game nothing.
// ---------------------------------------------------------------------------

namespace {

// ---------------------------------------------------------------------------
// Trampoline pointers
// ---------------------------------------------------------------------------
using FnWaveOutGetNumDevs  = UINT (WINAPI*)(void);
using FnWaveOutGetDevCapsW = MMRESULT (WINAPI*)(UINT_PTR, LPWAVEOUTCAPSW, UINT);
using FnWaveOutGetDevCapsA = MMRESULT (WINAPI*)(UINT_PTR, LPWAVEOUTCAPSA, UINT);
using FnWaveOutOpen        = MMRESULT (WINAPI*)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);

using FnWaveInGetNumDevs   = UINT (WINAPI*)(void);
using FnWaveInGetDevCapsW  = MMRESULT (WINAPI*)(UINT_PTR, LPWAVEINCAPSW, UINT);
using FnWaveInGetDevCapsA  = MMRESULT (WINAPI*)(UINT_PTR, LPWAVEINCAPSA, UINT);
using FnWaveInOpen         = MMRESULT (WINAPI*)(LPHWAVEIN, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);

// Not hooked — called directly, to ask winmm which endpoint a device index is.
using FnWaveOutMessage = MMRESULT (WINAPI*)(HWAVEOUT, UINT, DWORD_PTR, DWORD_PTR);
using FnWaveInMessage  = MMRESULT (WINAPI*)(HWAVEIN,  UINT, DWORD_PTR, DWORD_PTR);

// DirectSound's enumeration callbacks, declared locally: dsound.h would pull in
// the DirectX SDK headers and a dxguid.lib dependency for GUIDs this file never
// names, and only these two signatures are needed.
using PfnDsEnumA = BOOL (CALLBACK*)(GUID*, const char*,    const char*,    void*);
using PfnDsEnumW = BOOL (CALLBACK*)(GUID*, const wchar_t*, const wchar_t*, void*);

using FnDsEnumerateA = HRESULT (WINAPI*)(PfnDsEnumA, void*);
using FnDsEnumerateW = HRESULT (WINAPI*)(PfnDsEnumW, void*);

// MULTI_QI as far as CoCreateInstanceEx needs it. Only pItf is ever touched,
// and only when the interface asked for is the MMDevice enumerator.
struct AudioMultiQi {
    const IID* pIID;
    void*      pItf;
    HRESULT    hr;
};

using FnCoCreateInstance   = HRESULT (__stdcall*)(const CLSID*, void*, DWORD, const IID*, void**);
using FnCoCreateInstanceEx = HRESULT (__stdcall*)(const CLSID*, void*, DWORD, void*, DWORD, AudioMultiQi*);

using FnCoTaskMemFree  = void (__stdcall*)(void*);
using FnCoInitializeEx = HRESULT (__stdcall*)(void*, DWORD);
using FnCoUninitialize = void (__stdcall*)(void);

// cfgmgr32, for walking from an audio endpoint up to the hardware behind it.
// DEVINST is a DWORD; CONFIGRET is too.
using FnCmLocateDevNodeW              = DWORD (__stdcall*)(DWORD*, wchar_t*, ULONG);
using FnCmGetParent                   = DWORD (__stdcall*)(DWORD*, DWORD, ULONG);
using FnCmGetDeviceIdW                = DWORD (__stdcall*)(DWORD, wchar_t*, ULONG, ULONG);
using FnCmGetDevNodeRegistryPropertyW = DWORD (__stdcall*)(DWORD, ULONG, ULONG*, void*, ULONG*, ULONG);

FnWaveOutGetNumDevs  g_origWaveOutGetNumDevs  = nullptr;
FnWaveOutGetDevCapsW g_origWaveOutGetDevCapsW = nullptr;
FnWaveOutGetDevCapsA g_origWaveOutGetDevCapsA = nullptr;
FnWaveOutOpen        g_origWaveOutOpen        = nullptr;

FnWaveInGetNumDevs   g_origWaveInGetNumDevs   = nullptr;
FnWaveInGetDevCapsW  g_origWaveInGetDevCapsW  = nullptr;
FnWaveInGetDevCapsA  g_origWaveInGetDevCapsA  = nullptr;
FnWaveInOpen         g_origWaveInOpen         = nullptr;

FnWaveOutMessage     g_waveOutMessage         = nullptr;
FnWaveInMessage      g_waveInMessage          = nullptr;

FnDsEnumerateA       g_origDsEnumerateA        = nullptr;
FnDsEnumerateW       g_origDsEnumerateW        = nullptr;
FnDsEnumerateA       g_origDsCaptureEnumerateA = nullptr;
FnDsEnumerateW       g_origDsCaptureEnumerateW = nullptr;

FnCoCreateInstance   g_origCoCreateInstance   = nullptr;
FnCoCreateInstanceEx g_origCoCreateInstanceEx = nullptr;

// Resolved from ole32 directly, for our own use rather than as detour targets.
FnCoCreateInstance g_coCreateInstanceDirect = nullptr;
FnCoTaskMemFree    g_coTaskMemFree          = nullptr;
FnCoInitializeEx   g_coInitializeEx         = nullptr;
FnCoUninitialize   g_coUninitialize         = nullptr;

FnCmLocateDevNodeW              g_cmLocateDevNode   = nullptr;
FnCmGetParent                   g_cmGetParent       = nullptr;
FnCmGetDeviceIdW                g_cmGetDeviceId     = nullptr;
FnCmGetDevNodeRegistryPropertyW g_cmGetDevNodeProp  = nullptr;

// ---------------------------------------------------------------------------
// Core Audio ABI
//
// Declared here rather than via <mmdeviceapi.h> for the same reason dinput.cpp
// declares its own: the SDK GUIDs live in a static library this project does
// not link, and only a handful of vtable slots are ever called. The slot
// numbers below are the published interface layouts and have not changed since
// Windows Vista.
// ---------------------------------------------------------------------------
const CLSID kCLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
const IID   kIID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
const IID   kIID_IMMDeviceCollection =
    { 0x0BD7A1BE, 0x7A1A, 0x44DB, { 0x83, 0x97, 0xCC, 0x53, 0x92, 0x38, 0x7B, 0x5E } };
const IID   kIID_IMMEndpoint =
    { 0x1BE09788, 0x6894, 0x4089, { 0x85, 0x86, 0x9A, 0x2A, 0x6C, 0x26, 0x5A, 0xC5 } };
const IID   kIID_IUnknown =
    { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

struct AudioPropertyKey {
    GUID  fmtid;
    DWORD pid;
};

// PKEY_Device_FriendlyName / PKEY_Device_DeviceDesc share a format ID.
const AudioPropertyKey kPkeyFriendlyName =
    { { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14 };
const AudioPropertyKey kPkeyDeviceDesc =
    { { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 2 };

// PKEY_Device_InstanceId — the endpoint's device instance path, which is the
// handle cfgmgr32 needs to find the hardware behind it.
const AudioPropertyKey kPkeyInstanceId =
    { { 0x78C34FC8, 0x104A, 0x4ACA, { 0x9E, 0xA4, 0x52, 0x4D, 0x52, 0x99, 0x6E, 0x57 } }, 256 };

// PKEY_AudioEndpoint_GUID — the same GUID DirectSound enumerates the device
// under, which is what links a DirectSound entry back to an endpoint.
const AudioPropertyKey kPkeyEndpointGuid =
    { { 0x1DA5D803, 0xD492, 0x4EDD, { 0x8C, 0x23, 0xE0, 0xC0, 0xFF, 0xEE, 0x7F, 0x0E } }, 4 };

// PROPVARIANT as far as this file needs it. Only VT_LPWSTR is ever requested,
// and the value is released with CoTaskMemFree by hand rather than through
// PropVariantClear, which would add a link-time dependency for one call. The
// trailing padding makes this comfortably larger than a real PROPVARIANT on
// either architecture, so the property store cannot overrun it.
struct AudioPropVariant {
    unsigned short vt;
    unsigned short reserved1;
    unsigned short reserved2;
    unsigned short reserved3;
    void*          pointerValue;
    unsigned char  padding[32];
};

constexpr unsigned short kVtLpwstr = 31;

constexpr int     kDataFlowRender     = 0;
constexpr int     kDataFlowCapture    = 1;
constexpr int     kDataFlowAll        = 2;
constexpr DWORD   kDeviceStateActive  = 0x00000001;
constexpr DWORD   kClsCtxInprocServer = 0x00000001;
constexpr HRESULT kErrorNotFound      = static_cast<HRESULT>(0x80070490); // E_NOTFOUND

// DRV_RESERVED + 12 / + 13. winmm's only route to the endpoint behind an index.
constexpr UINT kDrvQueryDeviceInterfaceSize = 0x080C;
constexpr UINT kDrvQueryDeviceInterface     = 0x080D;

constexpr DWORD kCrSuccess       = 0;
constexpr ULONG kCmDrpHardwareId = 0x00000002;

// How far up the device tree to look for hardware IDs. An endpoint sits under
// the audio function device, which sits under the bus device (USB, HDAUDIO,
// PCI) — three or four levels is enough to reach the hardware a user would
// recognise, and stopping there keeps host controllers and hubs out of the
// match set.
constexpr int kDeviceTreeDepth = 5;

using PfnComQueryInterface = HRESULT (__stdcall*)(void*, const IID*, void**);
using PfnComAddRef         = ULONG   (__stdcall*)(void*);
using PfnComRelease        = ULONG   (__stdcall*)(void*);

using PfnEnumAudioEndpoints      = HRESULT (__stdcall*)(void*, int, DWORD, void**);
using PfnGetDefaultAudioEndpoint = HRESULT (__stdcall*)(void*, int, int, void**);
using PfnGetDevice               = HRESULT (__stdcall*)(void*, const wchar_t*, void**);
using PfnCollectionGetCount      = HRESULT (__stdcall*)(void*, UINT*);
using PfnCollectionItem          = HRESULT (__stdcall*)(void*, UINT, void**);
using PfnOpenPropertyStore       = HRESULT (__stdcall*)(void*, DWORD, void**);
using PfnDeviceGetId             = HRESULT (__stdcall*)(void*, wchar_t**);
using PfnEndpointGetDataFlow     = HRESULT (__stdcall*)(void*, int*);
using PfnPropertyStoreGetValue   = HRESULT (__stdcall*)(void*, const AudioPropertyKey*, AudioPropVariant*);

// The MMDevice enumerator vtable, patched once and reverted on unload.
void** g_endpointVTable = nullptr;

PfnEnumAudioEndpoints      g_origEnumAudioEndpoints      = nullptr;
PfnGetDefaultAudioEndpoint g_origGetDefaultAudioEndpoint = nullptr;
PfnGetDevice               g_origGetDevice               = nullptr;

// Filtered collections handed to the game and not yet released. Reported at
// unload: one still alive means a vtable pointing into this DLL outlives it.
LONG g_liveCollections = 0;

// ---------------------------------------------------------------------------
// COM primitives
// ---------------------------------------------------------------------------
template <typename Fn>
Fn Slot(void* object, int index)
{
    return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

void ComRelease(void* object)
{
    if (object)
        Slot<PfnComRelease>(object, 2)(object);
}

// Patch a single vtable slot in place. The same technique dinput.cpp uses, but
// deliberately not shared code: the two subsystems patch different vtables and
// revert independently.
void PatchSlot(void** vtable, int index, void* replacement, void** outOriginal)
{
    DWORD previousProtection = 0;

    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &previousProtection))
        return;

    if (outOriginal)
        *outOriginal = vtable[index];

    vtable[index] = replacement;

    VirtualProtect(&vtable[index], sizeof(void*), previousProtection, &previousProtection);
}

// Resolve ole32 and cache the handful of entry points used directly. Called
// both from hook installation (where ole32 is already in the process) and from
// the identity build, which may be the first thing in the process to want it.
HMODULE EnsureOle32()
{
    static HMODULE ole32 = nullptr;

    if (ole32)
        return ole32;

    ole32 = GetModuleHandleW(L"ole32.dll");

    if (!ole32)
        ole32 = LoadLibraryW(L"ole32.dll");

    if (!ole32)
        return nullptr;

    g_coTaskMemFree = reinterpret_cast<FnCoTaskMemFree>(
        reinterpret_cast<void*>(GetProcAddress(ole32, "CoTaskMemFree")));
    g_coInitializeEx = reinterpret_cast<FnCoInitializeEx>(
        reinterpret_cast<void*>(GetProcAddress(ole32, "CoInitializeEx")));
    g_coUninitialize = reinterpret_cast<FnCoUninitialize>(
        reinterpret_cast<void*>(GetProcAddress(ole32, "CoUninitialize")));
    g_coCreateInstanceDirect = reinterpret_cast<FnCoCreateInstance>(
        reinterpret_cast<void*>(GetProcAddress(ole32, "CoCreateInstance")));

    return ole32;
}

// Prefer the trampoline: going through our own hook would only re-patch a
// vtable that is already patched, and would run the detour's bookkeeping for an
// object the game never sees.
FnCoCreateInstance CoCreateDirect()
{
    return g_origCoCreateInstance ? g_origCoCreateInstance : g_coCreateInstanceDirect;
}

// COM on whatever thread the caller happens to be. S_FALSE means COM was
// already initialised this way and the reference count went up, so it still has
// to come back down; RPC_E_CHANGED_MODE means the thread is in the other
// apartment, which is usable for an in-proc object and is not ours to undo.
struct ComScope
{
    bool initialized = false;

    ComScope()
    {
        if (g_coInitializeEx)
            initialized = SUCCEEDED(g_coInitializeEx(nullptr, 0)); // COINIT_MULTITHREADED
    }

    ~ComScope()
    {
        if (initialized && g_coUninitialize)
            g_coUninitialize();
    }

    ComScope(const ComScope&)            = delete;
    ComScope& operator=(const ComScope&) = delete;
};

// Enumerate through the trampoline when the vtable is patched, so a caller that
// wants the unfiltered list (the identity build, the substitution logic and the
// export below) does not recurse into its own detour.
HRESULT RawEnumAudioEndpoints(void* enumerator, int dataFlow, DWORD stateMask, void** collection)
{
    if (g_origEnumAudioEndpoints)
        return g_origEnumAudioEndpoints(enumerator, dataFlow, stateMask, collection);

    return Slot<PfnEnumAudioEndpoints>(enumerator, 3)(enumerator, dataFlow, stateMask, collection);
}

HRESULT RawGetDefaultAudioEndpoint(void* enumerator, int dataFlow, int role, void** device)
{
    if (g_origGetDefaultAudioEndpoint)
        return g_origGetDefaultAudioEndpoint(enumerator, dataFlow, role, device);

    return Slot<PfnGetDefaultAudioEndpoint>(enumerator, 4)(enumerator, dataFlow, role, device);
}

// Run `body` against a fresh MMDevice enumerator. Returns false when Core Audio
// could not be reached at all, which is the one case every caller has to cope
// with rather than assume.
template <typename Fn>
bool WithMMDeviceEnumerator(Fn&& body)
{
    if (!EnsureOle32())
        return false;

    const FnCoCreateInstance create = CoCreateDirect();

    if (!create)
        return false;

    ComScope com;

    void* enumerator = nullptr;

    if (FAILED(create(&kCLSID_MMDeviceEnumerator, nullptr, kClsCtxInprocServer,
            &kIID_IMMDeviceEnumerator, &enumerator)) || !enumerator)
        return false;

    body(enumerator);

    ComRelease(enumerator);

    return true;
}

// ---------------------------------------------------------------------------
// Device property readers
// ---------------------------------------------------------------------------

// Read one VT_LPWSTR property off a device. Empty on any failure — a device
// that will not describe itself simply never matches an entry.
std::wstring ReadDeviceString(void* device, const AudioPropertyKey& key)
{
    if (!device)
        return {};

    void* store = nullptr;

    // OpenPropertyStore(STGM_READ = 0, &store)
    if (FAILED(Slot<PfnOpenPropertyStore>(device, 4)(device, 0, &store)) || !store)
        return {};

    AudioPropVariant value{};

    std::wstring result;

    if (SUCCEEDED(Slot<PfnPropertyStoreGetValue>(store, 5)(store, &key, &value)) &&
        value.vt == kVtLpwstr && value.pointerValue)
    {
        result = static_cast<const wchar_t*>(value.pointerValue);

        if (g_coTaskMemFree)
            g_coTaskMemFree(value.pointerValue);
    }

    ComRelease(store);

    return result;
}

std::wstring ReadDeviceId(void* device)
{
    if (!device)
        return {};

    wchar_t* raw = nullptr;

    if (FAILED(Slot<PfnDeviceGetId>(device, 5)(device, &raw)) || !raw)
        return {};

    std::wstring result(raw);

    if (g_coTaskMemFree)
        g_coTaskMemFree(raw);

    return result;
}

AudioDirection ReadDeviceDirection(void* device)
{
    if (!device)
        return AudioDirection::Unknown;

    void* endpoint = nullptr;

    if (FAILED(Slot<PfnComQueryInterface>(device, 0)(device, &kIID_IMMEndpoint, &endpoint)) || !endpoint)
        return AudioDirection::Unknown;

    int flow = -1;

    const bool ok = SUCCEEDED(Slot<PfnEndpointGetDataFlow>(endpoint, 3)(endpoint, &flow));

    ComRelease(endpoint);

    if (!ok)
        return AudioDirection::Unknown;

    if (flow == kDataFlowRender)  return AudioDirection::Output;
    if (flow == kDataFlowCapture) return AudioDirection::Input;

    return AudioDirection::Unknown;
}

// ---------------------------------------------------------------------------
// Hardware IDs
//
// An audio endpoint's own instance path (SWD\MMDEVAPI\...) says nothing about
// the hardware, so the IDs a user would recognise — USB\VID_0965&PID_9001,
// HDAUDIO\FUNC_01&VEN_10EC&DEV_0295 — come from its parents. cfgmgr32 is
// resolved by name: it is always present, and this avoids a link dependency for
// four calls.
// ---------------------------------------------------------------------------
bool EnsureCfgMgr()
{
    static bool tried = false;

    if (tried)
        return g_cmLocateDevNode != nullptr;

    tried = true;

    HMODULE cfgmgr = GetModuleHandleW(L"cfgmgr32.dll");

    if (!cfgmgr)
        cfgmgr = LoadLibraryW(L"cfgmgr32.dll");

    if (!cfgmgr)
        return false;

    g_cmLocateDevNode = reinterpret_cast<FnCmLocateDevNodeW>(
        reinterpret_cast<void*>(GetProcAddress(cfgmgr, "CM_Locate_DevNodeW")));
    g_cmGetParent = reinterpret_cast<FnCmGetParent>(
        reinterpret_cast<void*>(GetProcAddress(cfgmgr, "CM_Get_Parent")));
    g_cmGetDeviceId = reinterpret_cast<FnCmGetDeviceIdW>(
        reinterpret_cast<void*>(GetProcAddress(cfgmgr, "CM_Get_Device_IDW")));
    g_cmGetDevNodeProp = reinterpret_cast<FnCmGetDevNodeRegistryPropertyW>(
        reinterpret_cast<void*>(GetProcAddress(cfgmgr, "CM_Get_DevNode_Registry_PropertyW")));

    return g_cmLocateDevNode && g_cmGetParent && g_cmGetDeviceId && g_cmGetDevNodeProp;
}

// Append every REG_MULTI_SZ hardware ID of one device node.
void AppendHardwareIds(DWORD node, std::vector<std::wstring>& out)
{
    wchar_t buffer[2048]{};
    ULONG   bytes = sizeof(buffer);
    ULONG   type  = 0;

    if (g_cmGetDevNodeProp(node, kCmDrpHardwareId, &type, buffer, &bytes, 0) != kCrSuccess)
        return;

    const size_t limit = bytes / sizeof(wchar_t);

    for (size_t i = 0; i < limit && buffer[i]; )
    {
        std::wstring id(&buffer[i]);

        i += id.size() + 1;

        out.push_back(std::move(id));
    }
}

// Walk from the endpoint up the device tree, collecting each node's instance
// path and hardware IDs.
void CollectHardwareIds(const std::wstring& instanceId, std::vector<std::wstring>& out)
{
    if (instanceId.empty() || !EnsureCfgMgr())
        return;

    std::wstring mutableId = instanceId;   // CM_Locate_DevNodeW takes a non-const string

    DWORD node = 0;

    if (g_cmLocateDevNode(&node, mutableId.data(), 0) != kCrSuccess)   // CM_LOCATE_DEVNODE_NORMAL
        return;

    // The endpoint node itself is skipped: every audio endpoint on the machine
    // reports the same hardware ID (MMDEVAPI\AudioEndpoints) and the same
    // instance path already lives in identity.instanceId, so collecting it here
    // would only add a string that matches everything.
    for (int depth = 0; depth < kDeviceTreeDepth; ++depth)
    {
        if (depth > 0)
        {
            AppendHardwareIds(node, out);

            // The instance path is worth matching too: it carries the same
            // VID/PID as the hardware ID plus the physical port, which is how a
            // user pins one of two otherwise identical devices.
            wchar_t idBuffer[512]{};

            if (g_cmGetDeviceId(node, idBuffer, ARRAYSIZE(idBuffer), 0) == kCrSuccess)
                out.emplace_back(idBuffer);
        }

        DWORD parent = 0;

        if (g_cmGetParent(&parent, node, 0) != kCrSuccess)
            break;

        node = parent;
    }
}

// ---------------------------------------------------------------------------
// Device identity
// ---------------------------------------------------------------------------
struct AudioIdentity
{
    AudioDirection            direction = AudioDirection::Unknown;
    std::wstring              name;          // PKEY_Device_FriendlyName
    std::wstring              description;   // PKEY_Device_DeviceDesc
    std::wstring              endpointId;    // IMMDevice::GetId
    std::wstring              instanceId;    // PKEY_Device_InstanceId
    std::wstring              endpointGuid;  // PKEY_AudioEndpoint_GUID, i.e. the DirectSound GUID
    std::vector<std::wstring> hardwareIds;   // this device and its parents
};

AudioIdentity ReadIdentity(void* device)
{
    AudioIdentity identity;

    identity.direction    = ReadDeviceDirection(device);
    identity.name         = ReadDeviceString(device, kPkeyFriendlyName);
    identity.description  = ReadDeviceString(device, kPkeyDeviceDesc);
    identity.endpointId   = ReadDeviceId(device);
    identity.instanceId   = ReadDeviceString(device, kPkeyInstanceId);

    // MMDevAPI's endpoint property store does not carry PKEY_Device_InstanceId on
    // every Windows build, and without it there is no way into the device tree.
    // PnP names an endpoint SWD\MMDEVAPI\<endpoint id>, so it can be rebuilt from
    // the ID we always have.
    if (identity.instanceId.empty() && !identity.endpointId.empty())
        identity.instanceId = L"SWD\\MMDEVAPI\\" + identity.endpointId;
    identity.endpointGuid = ReadDeviceString(device, kPkeyEndpointGuid);

    CollectHardwareIds(identity.instanceId, identity.hardwareIds);

    return identity;
}

// ---------------------------------------------------------------------------
// The identity table
//
// Built once, from Core Audio, and never mutated afterwards — which is what
// lets lookups hand out bare pointers into it. winmm and DirectSound resolve
// through it; the Core Audio path reads identities directly and never touches
// it.
// ---------------------------------------------------------------------------
std::mutex                 g_identityMutex;
std::vector<AudioIdentity> g_identityRows;
bool                       g_identityBuilt = false;

// Guards against the build re-entering itself: the enumeration below can load
// and initialise the audio stack, and anything that comes back through a winmm
// hook on this thread must not wait on a table that is still being built.
thread_local bool t_buildingIdentity = false;

// Depth of "we are inside our own filtering work" on this thread.
//
// winmm and DirectSound are layered on top of Core Audio, so evaluating the
// filter for one of their devices makes them call IMMDeviceEnumerator behind
// our back — and the Core Audio detours would then apply the filter a second
// time, to plumbing rather than to a request the game made. Worse, GetDevice
// would answer E_NOTFOUND for a device winmm is in the middle of describing to
// us. While this is non-zero the Core Audio detours pass straight through.
thread_local int t_passThroughDepth = 0;

struct PassThroughScope
{
    PassThroughScope()  { ++t_passThroughDepth; }
    ~PassThroughScope() { --t_passThroughDepth; }

    PassThroughScope(const PassThroughScope&)            = delete;
    PassThroughScope& operator=(const PassThroughScope&) = delete;
};

inline bool PassingThrough() { return t_passThroughDepth > 0; }

std::vector<AudioIdentity> BuildIdentityTable()
{
    std::vector<AudioIdentity> rows;

    const bool reached = WithMMDeviceEnumerator([&rows](void* enumerator)
    {
        void* collection = nullptr;

        if (FAILED(RawEnumAudioEndpoints(enumerator, kDataFlowAll, kDeviceStateActive, &collection)) ||
            !collection)
            return;

        UINT count = 0;

        Slot<PfnCollectionGetCount>(collection, 3)(collection, &count);

        rows.reserve(count);

        for (UINT i = 0; i < count; ++i)
        {
            void* device = nullptr;

            if (FAILED(Slot<PfnCollectionItem>(collection, 4)(collection, i, &device)) || !device)
                continue;

            rows.push_back(ReadIdentity(device));

            ComRelease(device);
        }

        ComRelease(collection);
    });

    if (!reached)
        LogAudio(L"AUDIO", L"Core Audio unavailable",
            L"devices can only be matched on the name their own API reports");

    return rows;
}

const std::vector<AudioIdentity>& IdentityTable()
{
    static const std::vector<AudioIdentity> kEmpty;

    {
        std::lock_guard<std::mutex> lock(g_identityMutex);

        if (g_identityBuilt)
            return g_identityRows;
    }

    if (t_buildingIdentity)
        return kEmpty;

    t_buildingIdentity = true;

    std::vector<AudioIdentity> rows;

    {
        PassThroughScope passThrough;

        rows = BuildIdentityTable();
    }

    t_buildingIdentity = false;

    std::lock_guard<std::mutex> lock(g_identityMutex);

    if (!g_identityBuilt)
    {
        g_identityRows  = std::move(rows);
        g_identityBuilt = true;

        wchar_t detail[96]{};

        wsprintfW(detail, L"%u endpoints", static_cast<UINT>(g_identityRows.size()));

        LogAudio(L"AUDIO", L"identity table built", detail);
    }

    return g_identityRows;
}

// ---------------------------------------------------------------------------
// String matching
//
// Entries are literal, not regular expressions. A device name like
// "Speakers (NVIDIA Broadcast)" and a hardware ID like USB\VID_0965&PID_9001
// are full of characters a regex would read as syntax, and the point of the
// section is that a user can paste in what Windows showed them.
// ---------------------------------------------------------------------------
bool EqualsNoCase(const std::wstring& a, const std::wstring& b)
{
    return a.size() == b.size() &&
        std::equal(a.begin(), a.end(), b.begin(),
            [](wchar_t x, wchar_t y) { return towupper(x) == towupper(y); });
}

bool StartsWithNoCase(const std::wstring& text, const std::wstring& prefix)
{
    return !prefix.empty() && prefix.size() <= text.size() &&
        std::equal(prefix.begin(), prefix.end(), text.begin(),
            [](wchar_t x, wchar_t y) { return towupper(x) == towupper(y); });
}

bool ContainsNoCase(const std::wstring& text, const std::wstring& needle)
{
    if (needle.empty() || needle.size() > text.size())
        return false;

    return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
        [](wchar_t x, wchar_t y) { return towupper(x) == towupper(y); }) != text.end();
}

// ---------------------------------------------------------------------------
// Table lookups
// ---------------------------------------------------------------------------
const AudioIdentity* FindByEndpointId(const std::wstring& endpointId)
{
    if (endpointId.empty())
        return nullptr;

    for (const AudioIdentity& row : IdentityTable())
    {
        if (EqualsNoCase(row.endpointId, endpointId))
            return &row;
    }

    return nullptr;
}

// A DirectSound device GUID identifies an endpoint two ways: it is the
// endpoint's PKEY_AudioEndpoint_GUID, and it is also the tail of the endpoint
// ID ({0.0.0.00000000}.{that-guid}). The second is the fallback for a build
// that does not publish the property.
const AudioIdentity* FindByEndpointGuid(const std::wstring& guid)
{
    if (guid.empty())
        return nullptr;

    for (const AudioIdentity& row : IdentityTable())
    {
        if (EqualsNoCase(row.endpointGuid, guid))
            return &row;
    }

    for (const AudioIdentity& row : IdentityTable())
    {
        if (ContainsNoCase(row.endpointId, guid))
            return &row;
    }

    return nullptr;
}

// winmm truncates names to 31 characters and DirectSound has limits of its own,
// so the name a device reports may be a prefix of its real one. An ambiguous
// prefix resolves to nothing rather than to a guess — matching the wrong device
// is worse than falling back to the short name.
const AudioIdentity* FindByName(const std::wstring& name, AudioDirection direction)
{
    if (name.empty())
        return nullptr;

    const AudioIdentity* found = nullptr;

    for (const AudioIdentity& row : IdentityTable())
    {
        if (row.direction != direction)
            continue;

        if (!StartsWithNoCase(row.name, name) && !StartsWithNoCase(name, row.name))
            continue;

        if (found)
            return nullptr;

        found = &row;
    }

    return found;
}

// ---------------------------------------------------------------------------
// The filter
// ---------------------------------------------------------------------------
inline bool AudioActive() { return g_adEnabled && (g_adFilterOutput || g_adFilterInput); }

// The hooks are wanted when filtering is active OR when audio logging is on, so
// that every enumeration request can be logged even with nothing configured.
inline bool AudioHooksWanted() { return AudioActive() || g_logAudio; }

bool IdentityMatches(const AudioIdentity& identity, const std::wstring& entry)
{
    if (ContainsNoCase(identity.name, entry))         return true;
    if (ContainsNoCase(identity.description, entry))  return true;
    if (ContainsNoCase(identity.endpointId, entry))   return true;
    if (ContainsNoCase(identity.instanceId, entry))   return true;
    if (ContainsNoCase(identity.endpointGuid, entry)) return true;

    for (const std::wstring& hardwareId : identity.hardwareIds)
    {
        if (ContainsNoCase(hardwareId, entry))
            return true;
    }

    return false;
}

// A device is allowed when its direction is not filtered at all, or when it
// matches an entry in that direction's list. `reason` receives a short
// explanation for the Debug log.
bool DeviceAllowed(const AudioIdentity& identity, std::wstring& reason)
{
    if (!AudioActive())
        return true;

    const std::vector<std::wstring>* entries = nullptr;

    switch (identity.direction)
    {
    case AudioDirection::Output:
        if (!g_adFilterOutput) { reason = L"Output is not filtered"; return true; }
        entries = &g_adOutput;
        break;

    case AudioDirection::Input:
        if (!g_adFilterInput) { reason = L"Input is not filtered"; return true; }
        entries = &g_adInput;
        break;

    default:
        // Nothing said which direction this device carries, so no list governs
        // it. Showing it is the safer failure: the alternative silently hides a
        // device the user never asked to hide.
        reason = L"direction unknown, not filtered";
        return true;
    }

    for (const std::wstring& entry : *entries)
    {
        if (IdentityMatches(identity, entry))
        {
            reason = L"matched " + entry;
            return true;
        }

        if (g_logLevel >= LogLevel::Trace)
            LogAudioDiag(L"AUDIO RULE", identity.name.c_str(), (L"no match: " + entry).c_str());
    }

    reason = entries->empty()
        ? std::wstring(L"no devices are allowed in this direction")
        : std::wstring(L"no entry matched");

    return false;
}

void LogDevice(const wchar_t* verb, const AudioIdentity& identity, const std::wstring& reason)
{
    const std::wstring detail =
        std::wstring(AudioDirectionName(identity.direction)) + (reason.empty() ? L"" : L"  " + reason);

    LogAudioDiag(verb, identity.name.empty() ? L"(unnamed)" : identity.name.c_str(), detail.c_str());
}

// ---------------------------------------------------------------------------
// winmm — waveOut / waveIn
//
// These APIs address devices by index, so hiding one renumbers the rest. Every
// entry point that takes an index translates the game's index back to the real
// one; waveOutGetNumDevs / waveInGetNumDevs report how many survive.
// ---------------------------------------------------------------------------

// Name of real device `index`, read through the trampoline so the lookup does
// not re-enter the hook. Truncated to 31 characters by winmm itself.
std::wstring WaveDeviceName(bool output, UINT index)
{
    if (output)
    {
        WAVEOUTCAPSW caps{};

        if (!g_origWaveOutGetDevCapsW ||
            g_origWaveOutGetDevCapsW(index, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
            return {};

        caps.szPname[MAXPNAMELEN - 1] = L'\0';

        return caps.szPname;
    }

    WAVEINCAPSW caps{};

    if (!g_origWaveInGetDevCapsW ||
        g_origWaveInGetDevCapsW(index, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
        return {};

    caps.szPname[MAXPNAMELEN - 1] = L'\0';

    return caps.szPname;
}

// The Core Audio endpoint ID behind a winmm device index, extracted from the
// device interface path DRV_QUERYDEVICEINTERFACE hands back:
//
//   \\?\SWD#MMDEVAPI#{0.0.0.00000000}.{guid}#{interface-guid}
//                    ^------ endpoint ID ---^
//
// Empty when the driver does not answer the query, which is the case on the
// pre-Vista audio stack and for some virtual devices.
std::wstring WaveEndpointId(bool output, UINT index)
{
    if (output ? !g_waveOutMessage : !g_waveInMessage)
        return {};

    const auto handle = reinterpret_cast<void*>(static_cast<UINT_PTR>(index));

    ULONG bytes = 0;

    const MMRESULT sized = output
        ? g_waveOutMessage(static_cast<HWAVEOUT>(handle), kDrvQueryDeviceInterfaceSize,
              reinterpret_cast<DWORD_PTR>(&bytes), 0)
        : g_waveInMessage(static_cast<HWAVEIN>(handle), kDrvQueryDeviceInterfaceSize,
              reinterpret_cast<DWORD_PTR>(&bytes), 0);

    if (sized != MMSYSERR_NOERROR || bytes < sizeof(wchar_t) || bytes > 8192)
        return {};

    std::wstring path(bytes / sizeof(wchar_t) + 1, L'\0');

    const MMRESULT queried = output
        ? g_waveOutMessage(static_cast<HWAVEOUT>(handle), kDrvQueryDeviceInterface,
              reinterpret_cast<DWORD_PTR>(path.data()), bytes)
        : g_waveInMessage(static_cast<HWAVEIN>(handle), kDrvQueryDeviceInterface,
              reinterpret_cast<DWORD_PTR>(path.data()), bytes);

    if (queried != MMSYSERR_NOERROR)
        return {};

    path.resize(wcsnlen(path.c_str(), path.size()));

    const size_t marker = path.find(L"MMDEVAPI#");

    if (marker == std::wstring::npos)
        return {};

    const size_t start = marker + 9;   // past "MMDEVAPI#"
    const size_t end   = path.rfind(L'#');

    if (end == std::wstring::npos || end <= start)
        return {};

    return path.substr(start, end - start);
}

// Resolve a winmm device index to its full identity. Returns a pointer into the
// cached table when the device could be linked to a Core Audio endpoint;
// otherwise fills `fallback` with everything winmm alone can say and returns
// that, so a machine where the link fails still matches on the short name.
const AudioIdentity* WaveIdentity(bool output, UINT index, AudioIdentity& fallback)
{
    const AudioDirection direction = output ? AudioDirection::Output : AudioDirection::Input;

    const std::wstring endpointId = WaveEndpointId(output, index);

    if (const AudioIdentity* row = FindByEndpointId(endpointId))
        return row;

    fallback.direction  = direction;
    fallback.name       = WaveDeviceName(output, index);
    fallback.endpointId = endpointId;

    if (const AudioIdentity* row = FindByName(fallback.name, direction))
        return row;

    return &fallback;
}

UINT WaveRealCount(bool output)
{
    if (output)
        return g_origWaveOutGetNumDevs ? g_origWaveOutGetNumDevs() : 0;

    return g_origWaveInGetNumDevs ? g_origWaveInGetNumDevs() : 0;
}

bool WaveDeviceAllowed(bool output, UINT index, const AudioIdentity** outIdentity,
    AudioIdentity& storage, std::wstring& reason)
{
    const AudioIdentity* identity = WaveIdentity(output, index, storage);

    if (outIdentity)
        *outIdentity = identity;

    return DeviceAllowed(*identity, reason);
}

// Real index of the `virtualIndex`-th surviving device, or -1 when there is no
// such device.
int MapWaveIndex(bool output, UINT virtualIndex)
{
    const UINT count = WaveRealCount(output);

    UINT seen = 0;

    for (UINT i = 0; i < count; ++i)
    {
        AudioIdentity storage;
        std::wstring  reason;

        if (!WaveDeviceAllowed(output, i, nullptr, storage, reason))
            continue;

        if (seen == virtualIndex)
            return static_cast<int>(i);

        ++seen;
    }

    return -1;
}

// Count the survivors, logging each device on the way past. Only called from
// the GetNumDevs hooks, which is the one place per enumeration where a listing
// is worth writing.
UINT WaveAllowedCount(bool output)
{
    const UINT count = WaveRealCount(output);

    UINT allowed = 0;

    for (UINT i = 0; i < count; ++i)
    {
        const AudioIdentity* identity = nullptr;
        AudioIdentity        storage;
        std::wstring         reason;

        const bool keep = WaveDeviceAllowed(output, i, &identity, storage, reason);

        if (keep)
            ++allowed;

        LogDevice(keep ? L"AUDIO FOUND" : L"AUDIO HIDE", *identity, reason);
    }

    return allowed;
}

// Shared by waveOutGetDevCaps and waveInGetDevCaps, in both character sets.
// Returns true when the call should proceed with `deviceId` translated, false
// when it should fail with `result`.
//
// A device ID at or beyond the real count is left alone: waveOutGetDevCaps
// documents that an open HWAVEOUT may be passed in place of an ID, and a handle
// is a pointer-sized value far above any device count. Genuinely out-of-range
// integers take the same path and get the original's own error.
bool TranslateWaveDeviceId(bool output, UINT_PTR& deviceId, MMRESULT& result)
{
    if (!AudioActive())
        return true;

    if (deviceId == static_cast<UINT_PTR>(WAVE_MAPPER))
        return true;

    if (deviceId >= WaveRealCount(output))
        return true;

    const int mapped = MapWaveIndex(output, static_cast<UINT>(deviceId));

    if (mapped < 0)
    {
        result = MMSYSERR_BADDEVICEID;
        return false;
    }

    deviceId = static_cast<UINT_PTR>(mapped);

    return true;
}

UINT WINAPI HookWaveOutGetNumDevs(void)
{
    // winmm reaches Core Audio internally; do not filter its plumbing.
    PassThroughScope passThrough;

    const UINT real = g_origWaveOutGetNumDevs();

    if (!AudioActive())
    {
        LogAudio(L"AUDIO ENUM", L"waveOutGetNumDevs", nullptr);
        return real;
    }

    const UINT allowed = WaveAllowedCount(true);

    wchar_t detail[96]{};

    wsprintfW(detail, L"%u of %u output devices shown", allowed, real);

    LogAudio(L"AUDIO ENUM", L"waveOutGetNumDevs", detail);

    return allowed;
}

UINT WINAPI HookWaveInGetNumDevs(void)
{
    // winmm reaches Core Audio internally; do not filter its plumbing.
    PassThroughScope passThrough;

    const UINT real = g_origWaveInGetNumDevs();

    if (!AudioActive())
    {
        LogAudio(L"AUDIO ENUM", L"waveInGetNumDevs", nullptr);
        return real;
    }

    const UINT allowed = WaveAllowedCount(false);

    wchar_t detail[96]{};

    wsprintfW(detail, L"%u of %u input devices shown", allowed, real);

    LogAudio(L"AUDIO ENUM", L"waveInGetNumDevs", detail);

    return allowed;
}

MMRESULT WINAPI HookWaveOutGetDevCapsW(UINT_PTR deviceId, LPWAVEOUTCAPSW caps, UINT size)
{
    // winmm reaches Core Audio internally; do not filter its plumbing.
    PassThroughScope passThrough;

    MMRESULT failure = MMSYSERR_NOERROR;

    if (!TranslateWaveDeviceId(true, deviceId, failure))
        return failure;

    return g_origWaveOutGetDevCapsW(deviceId, caps, size);
}

MMRESULT WINAPI HookWaveOutGetDevCapsA(UINT_PTR deviceId, LPWAVEOUTCAPSA caps, UINT size)
{
    // winmm reaches Core Audio internally; do not filter its plumbing.
    PassThroughScope passThrough;

    MMRESULT failure = MMSYSERR_NOERROR;

    if (!TranslateWaveDeviceId(true, deviceId, failure))
        return failure;

    return g_origWaveOutGetDevCapsA(deviceId, caps, size);
}

MMRESULT WINAPI HookWaveInGetDevCapsW(UINT_PTR deviceId, LPWAVEINCAPSW caps, UINT size)
{
    // winmm reaches Core Audio internally; do not filter its plumbing.
    PassThroughScope passThrough;

    MMRESULT failure = MMSYSERR_NOERROR;

    if (!TranslateWaveDeviceId(false, deviceId, failure))
        return failure;

    return g_origWaveInGetDevCapsW(deviceId, caps, size);
}

MMRESULT WINAPI HookWaveInGetDevCapsA(UINT_PTR deviceId, LPWAVEINCAPSA caps, UINT size)
{
    // winmm reaches Core Audio internally; do not filter its plumbing.
    PassThroughScope passThrough;

    MMRESULT failure = MMSYSERR_NOERROR;

    if (!TranslateWaveDeviceId(false, deviceId, failure))
        return failure;

    return g_origWaveInGetDevCapsA(deviceId, caps, size);
}

// Resolve the device ID an Open call should really use.
//
// WAVE_MAPPER means "let Windows pick", and Windows picks the system default —
// which may be a device the filter hides. A filter that hides nothing of this
// direction therefore leaves it alone, and one that hides something steers it
// to the first survivor. The caller retries with WAVE_MAPPER if the steered
// open fails, because the mapper also performs format negotiation and a fixed
// device may not accept the format the game asked for.
//
// Returns false when the call should fail with `result`.
bool TranslateWaveOpenId(bool output, UINT& deviceId, bool& steeredFromMapper, MMRESULT& result)
{
    steeredFromMapper = false;

    if (!AudioActive())
        return true;

    if (deviceId != WAVE_MAPPER)
    {
        UINT_PTR wide = deviceId;

        if (!TranslateWaveDeviceId(output, wide, result))
            return false;

        deviceId = static_cast<UINT>(wide);

        return true;
    }

    const UINT real = WaveRealCount(output);

    UINT allowed = 0;

    for (UINT i = 0; i < real; ++i)
    {
        AudioIdentity storage;
        std::wstring  reason;

        if (WaveDeviceAllowed(output, i, nullptr, storage, reason))
            ++allowed;
    }

    if (allowed == real)
        return true; // nothing hidden — the mapper's choice is visible anyway

    const int mapped = MapWaveIndex(output, 0);

    if (mapped < 0)
    {
        result = MMSYSERR_BADDEVICEID;
        return false;
    }

    deviceId          = static_cast<UINT>(mapped);
    steeredFromMapper = true;

    return true;
}

MMRESULT WINAPI HookWaveOutOpen(LPHWAVEOUT handle, UINT deviceId, LPCWAVEFORMATEX format,
    DWORD_PTR callback, DWORD_PTR instance, DWORD flags)
{
    // winmm reaches Core Audio internally; do not filter its plumbing.
    PassThroughScope passThrough;

    MMRESULT failure    = MMSYSERR_NOERROR;
    bool     steered    = false;
    UINT     resolvedId = deviceId;

    if (!TranslateWaveOpenId(true, resolvedId, steered, failure))
        return failure;

    const MMRESULT result = g_origWaveOutOpen(handle, resolvedId, format, callback, instance, flags);

    if (result == MMSYSERR_NOERROR)
    {
        if (steered)
            LogAudio(L"AUDIO OPEN", L"waveOutOpen", L"WAVE_MAPPER steered to the first allowed device");

        return result;
    }

    // The steered device rejected the format the mapper would have negotiated.
    // Sound on the wrong device beats no sound at all.
    if (steered)
    {
        LogAudio(L"AUDIO OPEN", L"waveOutOpen",
            L"steered device rejected the format; falling back to WAVE_MAPPER");

        return g_origWaveOutOpen(handle, WAVE_MAPPER, format, callback, instance, flags);
    }

    return result;
}

MMRESULT WINAPI HookWaveInOpen(LPHWAVEIN handle, UINT deviceId, LPCWAVEFORMATEX format,
    DWORD_PTR callback, DWORD_PTR instance, DWORD flags)
{
    // winmm reaches Core Audio internally; do not filter its plumbing.
    PassThroughScope passThrough;

    MMRESULT failure    = MMSYSERR_NOERROR;
    bool     steered    = false;
    UINT     resolvedId = deviceId;

    if (!TranslateWaveOpenId(false, resolvedId, steered, failure))
        return failure;

    const MMRESULT result = g_origWaveInOpen(handle, resolvedId, format, callback, instance, flags);

    if (result == MMSYSERR_NOERROR)
    {
        if (steered)
            LogAudio(L"AUDIO OPEN", L"waveInOpen", L"WAVE_MAPPER steered to the first allowed device");

        return result;
    }

    if (steered)
    {
        LogAudio(L"AUDIO OPEN", L"waveInOpen",
            L"steered device rejected the format; falling back to WAVE_MAPPER");

        return g_origWaveInOpen(handle, WAVE_MAPPER, format, callback, instance, flags);
    }

    return result;
}

// ---------------------------------------------------------------------------
// DirectSound
//
// One thunk per character set sits between DirectSound and the caller's
// callback. The leading entry of every DirectSound enumeration is the default
// device, distinguished by a null GUID and named "Primary Sound Driver"; it is
// filtered like any other, which is usually what the user wants — a game that
// takes the first entry then lands on the device they chose rather than on the
// system default. List it by name to keep it.
// ---------------------------------------------------------------------------

struct DsEnumContext
{
    void*          userCallback = nullptr;
    void*          userRef      = nullptr;
    AudioDirection direction    = AudioDirection::Unknown;
    int            found        = 0;
    int            shown        = 0;
};

// Format a device GUID the way PKEY_AudioEndpoint_GUID reports it, or empty for
// the default-device entry, whose GUID is null.
std::wstring FormatDeviceGuid(const GUID* guid)
{
    if (!guid)
        return {};

    wchar_t buffer[48]{};

    wsprintfW(buffer, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        guid->Data1, guid->Data2, guid->Data3,
        guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
        guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);

    return buffer;
}

std::wstring AnsiToWide(const char* s)
{
    if (!s || !*s)
        return {};

    const int len = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);

    if (len <= 1)
        return {};

    std::wstring out(len - 1, L'\0');

    MultiByteToWideChar(CP_ACP, 0, s, -1, out.data(), len);

    return out;
}

// Resolve a DirectSound enumeration entry to its full identity, by the GUID
// first and the description second.
const AudioIdentity* DsIdentity(AudioDirection direction, const GUID* guid,
    const std::wstring& description, AudioIdentity& fallback)
{
    const std::wstring guidText = FormatDeviceGuid(guid);

    if (const AudioIdentity* row = FindByEndpointGuid(guidText))
        return row;

    if (const AudioIdentity* row = FindByName(description, direction))
        return row;

    fallback.direction    = direction;
    fallback.name         = description;
    fallback.endpointGuid = guidText;

    return &fallback;
}

// Shared verdict for both character sets.
bool DsDeviceAllowed(DsEnumContext* ctx, GUID* guid, const std::wstring& description)
{
    AudioIdentity storage;
    std::wstring  reason;

    const AudioIdentity* identity = DsIdentity(ctx->direction, guid, description, storage);

    const bool allowed = DeviceAllowed(*identity, reason);

    LogDevice(allowed ? L"AUDIO FOUND" : L"AUDIO HIDE", *identity, reason);

    return allowed;
}

BOOL CALLBACK DsEnumThunkA(GUID* guid, const char* description, const char* module, void* ref)
{
    DsEnumContext* ctx = static_cast<DsEnumContext*>(ref);

    ++ctx->found;

    if (!DsDeviceAllowed(ctx, guid, AnsiToWide(description)))
        return TRUE; // skip this device, keep enumerating

    ++ctx->shown;

    return reinterpret_cast<PfnDsEnumA>(ctx->userCallback)(guid, description, module, ctx->userRef);
}

BOOL CALLBACK DsEnumThunkW(GUID* guid, const wchar_t* description, const wchar_t* module, void* ref)
{
    DsEnumContext* ctx = static_cast<DsEnumContext*>(ref);

    ++ctx->found;

    if (!DsDeviceAllowed(ctx, guid, description ? description : L""))
        return TRUE;

    ++ctx->shown;

    return reinterpret_cast<PfnDsEnumW>(ctx->userCallback)(guid, description, module, ctx->userRef);
}

void LogDsSummary(const wchar_t* api, const DsEnumContext& ctx)
{
    wchar_t detail[96]{};

    wsprintfW(detail, L"%d found, %d shown", ctx.found, ctx.shown);

    LogAudio(L"AUDIO ENUM", api, detail);
}

// One body serves all four exports; only the trampoline, thunk and direction
// differ.
template <typename FnEnum, typename PfnThunk, typename PfnCallback>
HRESULT DsEnumerateImpl(FnEnum original, PfnThunk thunk, const wchar_t* api,
    AudioDirection direction, PfnCallback callback, void* ref)
{
    if (!callback)
        return E_INVALIDARG;

    // DirectSound sits on Core Audio too — same reasoning as winmm.
    PassThroughScope passThrough;

    if (!AudioActive())
    {
        LogAudio(L"AUDIO ENUM", api, nullptr);

        return original(callback, ref);
    }

    DsEnumContext ctx;

    ctx.userCallback = reinterpret_cast<void*>(callback);
    ctx.userRef      = ref;
    ctx.direction    = direction;

    const HRESULT hr = original(thunk, &ctx);

    LogDsSummary(api, ctx);

    return hr;
}

HRESULT WINAPI HookDirectSoundEnumerateA(PfnDsEnumA callback, void* ref)
{
    return DsEnumerateImpl(g_origDsEnumerateA, DsEnumThunkA, L"DirectSoundEnumerateA",
        AudioDirection::Output, callback, ref);
}

HRESULT WINAPI HookDirectSoundEnumerateW(PfnDsEnumW callback, void* ref)
{
    return DsEnumerateImpl(g_origDsEnumerateW, DsEnumThunkW, L"DirectSoundEnumerateW",
        AudioDirection::Output, callback, ref);
}

HRESULT WINAPI HookDirectSoundCaptureEnumerateA(PfnDsEnumA callback, void* ref)
{
    return DsEnumerateImpl(g_origDsCaptureEnumerateA, DsEnumThunkA, L"DirectSoundCaptureEnumerateA",
        AudioDirection::Input, callback, ref);
}

HRESULT WINAPI HookDirectSoundCaptureEnumerateW(PfnDsEnumW callback, void* ref)
{
    return DsEnumerateImpl(g_origDsCaptureEnumerateW, DsEnumThunkW, L"DirectSoundCaptureEnumerateW",
        AudioDirection::Input, callback, ref);
}

// ---------------------------------------------------------------------------
// Core Audio
//
// This path reads identities straight off the device and never consults the
// table, which is what keeps the table build from recursing into it.
// ---------------------------------------------------------------------------

struct EndpointVerdict
{
    AudioIdentity identity;
    bool          allowed = true;
    std::wstring  reason;
};

EndpointVerdict JudgeEndpoint(void* device)
{
    EndpointVerdict verdict;

    verdict.identity = ReadIdentity(device);
    verdict.allowed  = DeviceAllowed(verdict.identity, verdict.reason);

    return verdict;
}

// ---------------------------------------------------------------------------
// A collection of our own, holding only the endpoints that survived the filter.
//
// Handing back a private object is what keeps the per-collection survivor list
// out of a shared vtable: IMMDeviceCollection has exactly five methods, and
// owning the object means owning the lifetime of the references inside it.
// ---------------------------------------------------------------------------
struct FilteredCollection
{
    void**             vtable;   // must be first — this is a COM object
    LONG               refCount;
    std::vector<void*> devices;  // IMMDevice*, one reference held per entry
};

HRESULT __stdcall CollectionQueryInterface(void* self, const IID* iid, void** out)
{
    if (!out)
        return E_POINTER;

    *out = nullptr;

    if (!iid)
        return E_INVALIDARG;

    if (IsEqualGUID(*iid, kIID_IUnknown) || IsEqualGUID(*iid, kIID_IMMDeviceCollection))
    {
        InterlockedIncrement(&static_cast<FilteredCollection*>(self)->refCount);

        *out = self;

        return S_OK;
    }

    return E_NOINTERFACE;
}

ULONG __stdcall CollectionAddRef(void* self)
{
    return static_cast<ULONG>(InterlockedIncrement(&static_cast<FilteredCollection*>(self)->refCount));
}

ULONG __stdcall CollectionRelease(void* self)
{
    FilteredCollection* collection = static_cast<FilteredCollection*>(self);

    const LONG remaining = InterlockedDecrement(&collection->refCount);

    if (remaining == 0)
    {
        for (void* device : collection->devices)
            ComRelease(device);

        InterlockedDecrement(&g_liveCollections);

        delete collection;
    }

    return static_cast<ULONG>(remaining);
}

HRESULT __stdcall CollectionGetCount(void* self, UINT* count)
{
    if (!count)
        return E_POINTER;

    *count = static_cast<UINT>(static_cast<FilteredCollection*>(self)->devices.size());

    return S_OK;
}

HRESULT __stdcall CollectionItem(void* self, UINT index, void** device)
{
    if (!device)
        return E_POINTER;

    *device = nullptr;

    FilteredCollection* collection = static_cast<FilteredCollection*>(self);

    if (index >= collection->devices.size())
        return E_INVALIDARG;

    void* item = collection->devices[index];

    Slot<PfnComAddRef>(item, 1)(item);

    *device = item;

    return S_OK;
}

void* g_collectionVTable[] = {
    reinterpret_cast<void*>(&CollectionQueryInterface),
    reinterpret_cast<void*>(&CollectionAddRef),
    reinterpret_cast<void*>(&CollectionRelease),
    reinterpret_cast<void*>(&CollectionGetCount),
    reinterpret_cast<void*>(&CollectionItem),
};

// First endpoint of `dataFlow` that passes the filter, or null. Used to
// substitute a default the filter hides. The reference is the caller's.
void* FirstAllowedEndpoint(void* enumerator, int dataFlow)
{
    void* collection = nullptr;

    if (FAILED(RawEnumAudioEndpoints(enumerator, dataFlow, kDeviceStateActive, &collection)) ||
        !collection)
        return nullptr;

    UINT count = 0;

    Slot<PfnCollectionGetCount>(collection, 3)(collection, &count);

    void* found = nullptr;

    for (UINT i = 0; i < count && !found; ++i)
    {
        void* device = nullptr;

        if (FAILED(Slot<PfnCollectionItem>(collection, 4)(collection, i, &device)) || !device)
            continue;

        if (JudgeEndpoint(device).allowed)
            found = device;      // keep the reference for the caller
        else
            ComRelease(device);
    }

    ComRelease(collection);

    return found;
}

HRESULT __stdcall EnumAudioEndpointsDetour(void* self, int dataFlow, DWORD stateMask,
    void** collection)
{
    const HRESULT hr = g_origEnumAudioEndpoints(self, dataFlow, stateMask, collection);

    if (PassingThrough() || !AudioActive() || FAILED(hr) || !collection || !*collection)
    {
        LogAudio(L"AUDIO ENUM", L"IMMDeviceEnumerator::EnumAudioEndpoints", nullptr);

        return hr;
    }

    void* original = *collection;

    UINT count = 0;

    Slot<PfnCollectionGetCount>(original, 3)(original, &count);

    FilteredCollection* filtered = new FilteredCollection{ g_collectionVTable, 1, {} };

    for (UINT i = 0; i < count; ++i)
    {
        void* device = nullptr;

        if (FAILED(Slot<PfnCollectionItem>(original, 4)(original, i, &device)) || !device)
            continue;

        const EndpointVerdict verdict = JudgeEndpoint(device);

        LogDevice(verdict.allowed ? L"AUDIO FOUND" : L"AUDIO HIDE", verdict.identity, verdict.reason);

        if (verdict.allowed)
            filtered->devices.push_back(device);   // hand the reference to the collection
        else
            ComRelease(device);
    }

    ComRelease(original);

    InterlockedIncrement(&g_liveCollections);

    *collection = filtered;

    wchar_t detail[96]{};

    wsprintfW(detail, L"%u found, %u shown", count, static_cast<UINT>(filtered->devices.size()));

    LogAudio(L"AUDIO ENUM", L"IMMDeviceEnumerator::EnumAudioEndpoints", detail);

    return S_OK;
}

HRESULT __stdcall GetDefaultAudioEndpointDetour(void* self, int dataFlow, int role, void** device)
{
    const HRESULT hr = g_origGetDefaultAudioEndpoint(self, dataFlow, role, device);

    if (PassingThrough() || !AudioActive() || FAILED(hr) || !device || !*device)
        return hr;

    const EndpointVerdict verdict = JudgeEndpoint(*device);

    if (verdict.allowed)
    {
        LogAudioDiag(L"AUDIO DEFAULT", verdict.identity.name.c_str(), L"allowed");

        return hr;
    }

    // The system default is hidden, so hand the game the first device that is
    // not. Steering the default is the whole point of filtering for a game that
    // never enumerates at all.
    ComRelease(*device);

    *device = nullptr;

    void* substitute = FirstAllowedEndpoint(self, dataFlow);

    if (!substitute)
    {
        LogAudio(L"AUDIO DEFAULT", verdict.identity.name.c_str(),
            L"hidden by the filter and no allowed device to substitute");

        return kErrorNotFound;
    }

    LogAudio(L"AUDIO DEFAULT", verdict.identity.name.c_str(),
        (L"hidden; substituted " + ReadDeviceString(substitute, kPkeyFriendlyName)).c_str());

    *device = substitute;

    return S_OK;
}

HRESULT __stdcall GetDeviceDetour(void* self, const wchar_t* id, void** device)
{
    const HRESULT hr = g_origGetDevice(self, id, device);

    if (PassingThrough() || !AudioActive() || FAILED(hr) || !device || !*device)
        return hr;

    const EndpointVerdict verdict = JudgeEndpoint(*device);

    if (verdict.allowed)
        return hr;

    LogAudio(L"AUDIO HIDE", verdict.identity.name.c_str(),
        L"requested by ID and hidden by the filter");

    ComRelease(*device);

    *device = nullptr;

    return kErrorNotFound;
}

// Patch slots 3/4/5 of the IMMDeviceEnumerator vtable. MMDevAPI hands every
// caller the same vtable, so this runs once for the lifetime of the process.
void AttachEndpointFilter(void* enumerator)
{
    if (!enumerator || g_endpointVTable)
        return;

    void** vtable = *static_cast<void***>(enumerator);

    if (!vtable)
        return;

    PatchSlot(vtable, 3, reinterpret_cast<void*>(&EnumAudioEndpointsDetour),
        reinterpret_cast<void**>(&g_origEnumAudioEndpoints));
    PatchSlot(vtable, 4, reinterpret_cast<void*>(&GetDefaultAudioEndpointDetour),
        reinterpret_cast<void**>(&g_origGetDefaultAudioEndpoint));
    PatchSlot(vtable, 5, reinterpret_cast<void*>(&GetDeviceDetour),
        reinterpret_cast<void**>(&g_origGetDevice));

    g_endpointVTable = vtable;

    LogAudio(L"AUDIO HOOK", L"IMMDeviceEnumerator", L"endpoint enumeration filtered");
}

// A caller that asked for IUnknown still has the enumerator behind it. Query
// for the real interface, patch the vtable that comes back — MMDevAPI shares it
// with the caller's pointer — and drop our reference again.
void AttachEndpointFilterVia(void* unknown)
{
    if (!unknown || g_endpointVTable)
        return;

    void* enumerator = nullptr;

    if (FAILED(Slot<PfnComQueryInterface>(unknown, 0)(unknown, &kIID_IMMDeviceEnumerator, &enumerator)) ||
        !enumerator)
        return;

    AttachEndpointFilter(enumerator);

    ComRelease(enumerator);
}

HRESULT __stdcall HookCoCreateInstance(const CLSID* clsid, void* outer, DWORD context,
    const IID* iid, void** out)
{
    const HRESULT hr = g_origCoCreateInstance(clsid, outer, context, iid, out);

    if (FAILED(hr) || !clsid || !iid || !out || !*out)
        return hr;

    if (!IsEqualGUID(*clsid, kCLSID_MMDeviceEnumerator))
        return hr;

    if (IsEqualGUID(*iid, kIID_IMMDeviceEnumerator))
        AttachEndpointFilter(*out);
    else if (IsEqualGUID(*iid, kIID_IUnknown))
        AttachEndpointFilterVia(*out);

    return hr;
}

HRESULT __stdcall HookCoCreateInstanceEx(const CLSID* clsid, void* outer, DWORD context,
    void* serverInfo, DWORD count, AudioMultiQi* results)
{
    const HRESULT hr = g_origCoCreateInstanceEx(clsid, outer, context, serverInfo, count, results);

    if (FAILED(hr) || !clsid || !results || !IsEqualGUID(*clsid, kCLSID_MMDeviceEnumerator))
        return hr;

    for (DWORD i = 0; i < count; ++i)
    {
        if (FAILED(results[i].hr) || !results[i].pItf || !results[i].pIID)
            continue;

        if (IsEqualGUID(*results[i].pIID, kIID_IMMDeviceEnumerator))
            AttachEndpointFilter(results[i].pItf);
        else if (IsEqualGUID(*results[i].pIID, kIID_IUnknown))
            AttachEndpointFilterVia(results[i].pItf);
    }

    return hr;
}

// ---------------------------------------------------------------------------
// Hook installation
//
// Each group installs independently and is safe to call repeatedly:
// MH_ERROR_ALREADY_CREATED is ignored by LogHookInit, and
// MH_ERROR_MODULE_NOT_FOUND at startup just means the module was not loaded yet
// — LateInstallAudioHooks retries when it appears.
// ---------------------------------------------------------------------------
void InstallWinmmHooks()
{
    LogHookInit(L"winmm.dll", "waveOutGetNumDevs",
        MH_CreateHookApi(L"winmm.dll", "waveOutGetNumDevs",
            reinterpret_cast<LPVOID>(HookWaveOutGetNumDevs),
            reinterpret_cast<LPVOID*>(&g_origWaveOutGetNumDevs)));

    LogHookInit(L"winmm.dll", "waveOutGetDevCapsW",
        MH_CreateHookApi(L"winmm.dll", "waveOutGetDevCapsW",
            reinterpret_cast<LPVOID>(HookWaveOutGetDevCapsW),
            reinterpret_cast<LPVOID*>(&g_origWaveOutGetDevCapsW)));

    LogHookInit(L"winmm.dll", "waveOutGetDevCapsA",
        MH_CreateHookApi(L"winmm.dll", "waveOutGetDevCapsA",
            reinterpret_cast<LPVOID>(HookWaveOutGetDevCapsA),
            reinterpret_cast<LPVOID*>(&g_origWaveOutGetDevCapsA)));

    LogHookInit(L"winmm.dll", "waveOutOpen",
        MH_CreateHookApi(L"winmm.dll", "waveOutOpen",
            reinterpret_cast<LPVOID>(HookWaveOutOpen),
            reinterpret_cast<LPVOID*>(&g_origWaveOutOpen)));

    LogHookInit(L"winmm.dll", "waveInGetNumDevs",
        MH_CreateHookApi(L"winmm.dll", "waveInGetNumDevs",
            reinterpret_cast<LPVOID>(HookWaveInGetNumDevs),
            reinterpret_cast<LPVOID*>(&g_origWaveInGetNumDevs)));

    LogHookInit(L"winmm.dll", "waveInGetDevCapsW",
        MH_CreateHookApi(L"winmm.dll", "waveInGetDevCapsW",
            reinterpret_cast<LPVOID>(HookWaveInGetDevCapsW),
            reinterpret_cast<LPVOID*>(&g_origWaveInGetDevCapsW)));

    LogHookInit(L"winmm.dll", "waveInGetDevCapsA",
        MH_CreateHookApi(L"winmm.dll", "waveInGetDevCapsA",
            reinterpret_cast<LPVOID>(HookWaveInGetDevCapsA),
            reinterpret_cast<LPVOID*>(&g_origWaveInGetDevCapsA)));

    LogHookInit(L"winmm.dll", "waveInOpen",
        MH_CreateHookApi(L"winmm.dll", "waveInOpen",
            reinterpret_cast<LPVOID>(HookWaveInOpen),
            reinterpret_cast<LPVOID*>(&g_origWaveInOpen)));

    // Not hooked, just resolved: this is how a device index is linked back to a
    // Core Audio endpoint.
    if (HMODULE winmm = GetModuleHandleW(L"winmm.dll"))
    {
        g_waveOutMessage = reinterpret_cast<FnWaveOutMessage>(
            reinterpret_cast<void*>(GetProcAddress(winmm, "waveOutMessage")));
        g_waveInMessage = reinterpret_cast<FnWaveInMessage>(
            reinterpret_cast<void*>(GetProcAddress(winmm, "waveInMessage")));
    }
}

void InstallDirectSoundHooks()
{
    LogHookInit(L"dsound.dll", "DirectSoundEnumerateA",
        MH_CreateHookApi(L"dsound.dll", "DirectSoundEnumerateA",
            reinterpret_cast<LPVOID>(HookDirectSoundEnumerateA),
            reinterpret_cast<LPVOID*>(&g_origDsEnumerateA)));

    LogHookInit(L"dsound.dll", "DirectSoundEnumerateW",
        MH_CreateHookApi(L"dsound.dll", "DirectSoundEnumerateW",
            reinterpret_cast<LPVOID>(HookDirectSoundEnumerateW),
            reinterpret_cast<LPVOID*>(&g_origDsEnumerateW)));

    LogHookInit(L"dsound.dll", "DirectSoundCaptureEnumerateA",
        MH_CreateHookApi(L"dsound.dll", "DirectSoundCaptureEnumerateA",
            reinterpret_cast<LPVOID>(HookDirectSoundCaptureEnumerateA),
            reinterpret_cast<LPVOID*>(&g_origDsCaptureEnumerateA)));

    LogHookInit(L"dsound.dll", "DirectSoundCaptureEnumerateW",
        MH_CreateHookApi(L"dsound.dll", "DirectSoundCaptureEnumerateW",
            reinterpret_cast<LPVOID>(HookDirectSoundCaptureEnumerateW),
            reinterpret_cast<LPVOID*>(&g_origDsCaptureEnumerateW)));
}

void InstallCoreAudioHooks()
{
    EnsureOle32();

    LogHookInit(L"ole32.dll", "CoCreateInstance",
        MH_CreateHookApi(L"ole32.dll", "CoCreateInstance",
            reinterpret_cast<LPVOID>(HookCoCreateInstance),
            reinterpret_cast<LPVOID*>(&g_origCoCreateInstance)));

    LogHookInit(L"ole32.dll", "CoCreateInstanceEx",
        MH_CreateHookApi(L"ole32.dll", "CoCreateInstanceEx",
            reinterpret_cast<LPVOID>(HookCoCreateInstanceEx),
            reinterpret_cast<LPVOID*>(&g_origCoCreateInstanceEx)));
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void InstallAudioHooks()
{
    if (!AudioHooksWanted())
    {
        InterposerLog(L"HOOK INIT", L"audio hooks skipped (Logging.Audio and AudioDevices both unset)");
        return;
    }

    InstallWinmmHooks();
    InstallDirectSoundHooks();
    InstallCoreAudioHooks();
}

void LateInstallAudioHooks(const wchar_t* moduleName)
{
    if (!moduleName || !AudioHooksWanted())
        return;

    if (_wcsicmp(moduleName, L"winmm.dll") == 0)
    {
        InstallWinmmHooks();
        MH_EnableHook(MH_ALL_HOOKS);
    }
    else if (_wcsicmp(moduleName, L"dsound.dll") == 0)
    {
        InstallDirectSoundHooks();
        MH_EnableHook(MH_ALL_HOOKS);
    }
    else if (_wcsicmp(moduleName, L"ole32.dll") == 0)
    {
        InstallCoreAudioHooks();
        MH_EnableHook(MH_ALL_HOOKS);
    }
}

void RemoveAudioHooks()
{
    // MinHook knows nothing about the vtable slots, and an MMDevAPI vtable
    // still pointing into this DLL after it unloads would fault.
    if (g_endpointVTable)
    {
        if (g_origEnumAudioEndpoints)
            PatchSlot(g_endpointVTable, 3, reinterpret_cast<void*>(g_origEnumAudioEndpoints), nullptr);

        if (g_origGetDefaultAudioEndpoint)
            PatchSlot(g_endpointVTable, 4, reinterpret_cast<void*>(g_origGetDefaultAudioEndpoint), nullptr);

        if (g_origGetDevice)
            PatchSlot(g_endpointVTable, 5, reinterpret_cast<void*>(g_origGetDevice), nullptr);

        g_endpointVTable              = nullptr;
        g_origEnumAudioEndpoints      = nullptr;
        g_origGetDefaultAudioEndpoint = nullptr;
        g_origGetDevice               = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(g_identityMutex);

        g_identityRows.clear();
        g_identityBuilt = false;
    }

    // A filtered collection the game still holds has a vtable in this image.
    // Nothing can be done about it here, but it should not go unreported.
    const LONG live = InterlockedCompareExchange(&g_liveCollections, 0, 0);

    if (live > 0)
    {
        wchar_t detail[96]{};

        wsprintfW(detail, L"%ld filtered device collections still referenced at unload", live);

        InterposerLog(L"AUDIO", detail);
    }
}

// ---------------------------------------------------------------------------
// Unified audio device enumeration — see audio.h for the contract. Reports the
// real device set with the same allowed/hidden decision the hooks use, so a
// picker UI can offer every device, show which ones the game sees, and hand the
// user the exact strings an AudioDevices entry accepts.
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) DWORD InterposerEnumAudioDevices(
    InterposerAudioDevice* buffer, DWORD capacity, DWORD* outCount)
{
    if (outCount)
        *outCount = 0;

    DWORD total = 0;

    // The export is reachable from a host thread that never initialised COM, so
    // WithMMDeviceEnumerator resolves and initialises ole32 rather than assuming
    // either. Unlike LoadConfig this never runs under the loader lock.
    WithMMDeviceEnumerator([&](void* enumerator)
    {
        // The default endpoints, so each device can be flagged. Failure here
        // just means nothing is marked default.
        std::wstring defaultRenderId;
        std::wstring defaultCaptureId;

        void* defaultDevice = nullptr;

        // eConsole
        if (SUCCEEDED(RawGetDefaultAudioEndpoint(enumerator, kDataFlowRender, 0, &defaultDevice)) &&
            defaultDevice)
        {
            defaultRenderId = ReadDeviceId(defaultDevice);

            ComRelease(defaultDevice);

            defaultDevice = nullptr;
        }

        if (SUCCEEDED(RawGetDefaultAudioEndpoint(enumerator, kDataFlowCapture, 0, &defaultDevice)) &&
            defaultDevice)
        {
            defaultCaptureId = ReadDeviceId(defaultDevice);

            ComRelease(defaultDevice);

            defaultDevice = nullptr;
        }

        void* collection = nullptr;

        if (FAILED(RawEnumAudioEndpoints(enumerator, kDataFlowAll, kDeviceStateActive, &collection)) ||
            !collection)
            return;

        UINT count = 0;

        Slot<PfnCollectionGetCount>(collection, 3)(collection, &count);

        for (UINT i = 0; i < count; ++i)
        {
            void* device = nullptr;

            if (FAILED(Slot<PfnCollectionItem>(collection, 4)(collection, i, &device)) || !device)
                continue;

            const EndpointVerdict verdict = JudgeEndpoint(device);

            ComRelease(device);

            if (buffer && total < capacity)
            {
                const AudioIdentity& id = verdict.identity;

                InterposerAudioDevice& out = buffer[total];

                ZeroMemory(&out, sizeof(out));

                lstrcpynW(out.name,        id.name.c_str(),        ARRAYSIZE(out.name));
                lstrcpynW(out.description, id.description.c_str(), ARRAYSIZE(out.description));
                lstrcpynW(out.id,          id.endpointId.c_str(),  ARRAYSIZE(out.id));
                lstrcpynW(out.instanceId,  id.instanceId.c_str(),  ARRAYSIZE(out.instanceId));

                // The most specific hardware ID is the first one reported by the
                // nearest node that has any, which is what Windows orders them by.
                if (!id.hardwareIds.empty())
                    lstrcpynW(out.hardwareId, id.hardwareIds.front().c_str(), ARRAYSIZE(out.hardwareId));

                out.direction = (id.direction == AudioDirection::Output) ? 1
                              : (id.direction == AudioDirection::Input)  ? 2
                              : 0;

                out.isDefault = (!id.endpointId.empty() &&
                                 (id.endpointId == defaultRenderId ||
                                  id.endpointId == defaultCaptureId)) ? 1 : 0;

                out.allowed = verdict.allowed ? 1 : 0;

                if (outCount)
                    *outCount = total + 1;
            }

            ++total;
        }

        ComRelease(collection);
    });

    return total;
}
