#include "dinput.h"
#include "config.h"

#include <MinHook.h>

#include <string>

// ---------------------------------------------------------------------------
// DirectInput ABI
//
// Declared here rather than including the Windows SDK <dinput.h>: only a few
// structures and a handful of GUIDs are needed, taking the SDK header would add
// a dxguid.lib link dependency for the GUIDs, and dll\src is on the include
// path — so <dinput.h> would resolve to this subsystem's own header instead.
//
// Natural alignment throughout, matching DirectInput itself. The DX3 record is
// 16 bytes on both architectures; the DX8 record is 20 on x86 and 24 on x64,
// because uAppData is pointer-sized. Nothing here hardcodes those numbers.
// ---------------------------------------------------------------------------
namespace {

struct DiDeviceInstanceA
{
    DWORD dwSize;
    GUID  guidInstance;
    GUID  guidProduct;
    DWORD dwDevType;
    CHAR  tszInstanceName[MAX_PATH];
    CHAR  tszProductName[MAX_PATH];
    GUID  guidFFDriver;
    WORD  wUsagePage;
    WORD  wUsage;
};

struct DiDeviceInstanceW
{
    DWORD dwSize;
    GUID  guidInstance;
    GUID  guidProduct;
    DWORD dwDevType;
    WCHAR tszInstanceName[MAX_PATH];
    WCHAR tszProductName[MAX_PATH];
    GUID  guidFFDriver;
    WORD  wUsagePage;
    WORD  wUsage;
};

// The sizes are part of the ABI — DirectInput dispatches on dwSize — and are
// identical in DirectInput 7 and 8.
static_assert(sizeof(DiDeviceInstanceA) == 580,  "DIDEVICEINSTANCEA must be 580 bytes");
static_assert(sizeof(DiDeviceInstanceW) == 1100, "DIDEVICEINSTANCEW must be 1100 bytes");

// DIDEVICEOBJECTDATA_DX3 — what a pre-DX8 game passes and expects back.
struct DiObjectDataDx3
{
    DWORD dwOfs;
    DWORD dwData;
    DWORD dwTimeStamp;
    DWORD dwSequence;
};

// DIDEVICEOBJECTDATA — gained uAppData in DX8.
struct DiObjectDataDx8
{
    DWORD    dwOfs;
    DWORD    dwData;
    DWORD    dwTimeStamp;
    DWORD    dwSequence;
    UINT_PTR uAppData;
};

static_assert(sizeof(DiObjectDataDx3) == 16, "DIDEVICEOBJECTDATA_DX3 must be 16 bytes");
static_assert(sizeof(InterposerInputEvent) == sizeof(DiObjectDataDx3),
    "the plugin-facing event must match the DX3 record — the queue copies between them");
static_assert(sizeof(DiObjectDataDx8) != sizeof(DiObjectDataDx3),
    "the DX3 and DX8 record sizes must differ — cbObjectData is how the two are told apart");

using PfnEnumCallbackA  = BOOL    (WINAPI*)(const DiDeviceInstanceA*, void*);
using PfnEnumCallbackW  = BOOL    (WINAPI*)(const DiDeviceInstanceW*, void*);
using PfnEnumDevicesA   = HRESULT (WINAPI*)(void*, DWORD, PfnEnumCallbackA, void*, DWORD);
using PfnEnumDevicesW   = HRESULT (WINAPI*)(void*, DWORD, PfnEnumCallbackW, void*, DWORD);
using PfnCreateDevice   = HRESULT (WINAPI*)(void*, const GUID*, void**, void*);
using PfnGetDeviceData  = HRESULT (WINAPI*)(void*, DWORD, void*, DWORD*, DWORD);
using PfnSetDataFormat  = HRESULT (WINAPI*)(void*, const void*);
using PfnQueryInterface = HRESULT (WINAPI*)(void*, const GUID*, void**);
using PfnAddRef         = ULONG   (WINAPI*)(void*);

using PfnDirectInput8Create  = HRESULT (WINAPI*)(HINSTANCE, DWORD, const GUID*, void**, void*);
using PfnDirectInputCreateA  = HRESULT (WINAPI*)(HINSTANCE, DWORD, void**, void*);
using PfnDirectInputCreateEx = HRESULT (WINAPI*)(HINSTANCE, DWORD, const GUID*, void**, void*);

const GUID kIID_IDirectInput8A =
    { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
const GUID kIID_IDirectInput8W =
    { 0xBF798031, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
const GUID kGUID_SysMouse =
    { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
const GUID kGUID_SysKeyboard =
    { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

// Pre-DX8 interface IIDs the game asks for. A game creates a device and then
// QueryInterfaces it for IDirectInputDevice2A, because that is where Poll()
// lives. A DX8 object answers E_NOINTERFACE, so the game silently discards the
// device and gameplay ends up with no input at all — menus still work, because
// those run on Win32 messages. Answering these IIDs with the same object is
// safe: the DX8 vtable is layout-compatible for every slot such a game uses.
const GUID kIID_IDirectInputDeviceA =
    { 0x5944E680, 0xC92E, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
const GUID kIID_IDirectInputDevice2A =
    { 0x5944E682, 0xC92E, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
const GUID kIID_IDirectInputDevice7A =
    { 0x57D7C6BC, 0x2356, 0x11D3, { 0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE } };
const GUID kIID_IDirectInputA =
    { 0x89521360, 0xAA8A, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
const GUID kIID_IDirectInput2A =
    { 0x5944E662, 0xAA8A, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
const GUID kIID_IDirectInput7A =
    { 0x9A4CB684, 0x236D, 0x11D3, { 0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE } };

// Device types exactly as DirectInput 7 reported them, used when the
// enumeration is answered directly rather than run.
constexpr DWORD kDevTypeMouseTraditional = 0x0102; // DIDEVTYPEMOUSE_TRADITIONAL
constexpr DWORD kDevTypeKeyboardPcEnh    = 0x0403; // DIDEVTYPEKEYBOARD_PCENH

// The class the caller asks EnumDevices to restrict itself to. DIDEVTYPE_MOUSE
// / _KEYBOARD and DI8DEVCLASS_POINTER / _KEYBOARD share these values, which is
// why one check covers both eras.
constexpr DWORD kDevClassAll      = 0;
constexpr DWORD kDevClassMouse    = 2;
constexpr DWORD kDevClassKeyboard = 3;

// Events pulled from DirectInput per GetDeviceData call. Sized so the scratch
// buffer stays comfortably on the stack (3 KB at worst).
constexpr DWORD kEventBatch = 128;

// Events held over between calls when a mouse transform is registered. Has to
// exceed kEventBatch: a transform is allowed to return more events than it was
// given, and whatever the game did not ask for this call is kept for the next.
constexpr DWORD kPendingMax = 192;

// The axis GUIDs DirectInput reports in a data format, identified by Data1
// alone — that is enough to tell them apart and avoids carrying three more
// GUID constants.
constexpr DWORD kGuidXAxisData1 = 0xA36D02E0u;
constexpr DWORD kGuidYAxisData1 = 0xA36D02E1u;
constexpr DWORD kGuidZAxisData1 = 0xA36D02E2u;

// ---------------------------------------------------------------------------
// State
//
// The trampolines below are captured once per process: every object of a given
// interface shares one dinput8 vtable, so the first patch is the only one.
// Creation happens on the engine's init thread before any other thread exists
// in practice, and each pointer is written once and only read afterwards.
// ---------------------------------------------------------------------------
HMODULE               g_di8       = nullptr;
PfnDirectInput8Create g_di8Create = nullptr;

PfnCreateDevice   g_origCreateDevice  = nullptr;
PfnQueryInterface g_origDirectInputQI = nullptr;

PfnEnumDevicesA g_origEnumDevicesA = nullptr;
PfnEnumDevicesW g_origEnumDevicesW = nullptr;

PfnGetDeviceData   g_origGetDeviceData  = nullptr;
PfnQueryInterface  g_origDeviceQI       = nullptr;
PfnSetDataFormat   g_origSetDataFormat  = nullptr;

// Mouse transform state. The mouse and keyboard share one device vtable, so
// every device detour runs for both — anything mouse-specific has to be gated
// on the object actually being the mouse.
InterposerMouseTransform g_mouseTransform     = nullptr;
void*                    g_mouseTransformUser = nullptr;
void*                    g_mouseDevice        = nullptr;

DWORD g_axisOffsetX = INTERPOSER_MOUSE_AXIS_NONE;
DWORD g_axisOffsetY = INTERPOSER_MOUSE_AXIS_NONE;
DWORD g_axisOffsetZ = INTERPOSER_MOUSE_AXIS_NONE;

// Events the transform produced that the game has not collected yet. Owned by
// the mouse alone, so a keyboard read can never drain mouse events.
InterposerInputEvent g_pending[kPendingMax];
DWORD                g_pendingCount = 0;

// The vtables the slots above were taken from. Kept so the patches can be
// reverted on detach — MinHook only knows about its own trampolines, and a
// vtable still pointing into this DLL after it unloads would fault on the next
// call. All belong to dinput8.dll and outlive us.
void** g_bridgeVTable = nullptr;  // IDirectInput8A,       slots 0 and 3
void** g_enumVTableA  = nullptr;  // IDirectInput8A,       slot 4
void** g_enumVTableW  = nullptr;  // IDirectInput8W,       slot 4
void** g_deviceVTable = nullptr;  // IDirectInputDevice8A, slots 0 and 10

PfnDirectInputCreateA  g_origDirectInputCreateA  = nullptr;
PfnDirectInputCreateEx g_origDirectInputCreateEx = nullptr;
PfnDirectInput8Create  g_origDirectInput8Create  = nullptr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool GuidEquals(const GUID* a, const GUID* b)
{
    return a && b && memcmp(a, b, sizeof(GUID)) == 0;
}

bool IsAnsiDirectInputIid(const GUID* iid)
{
    return GuidEquals(iid, &kIID_IDirectInputA)
        || GuidEquals(iid, &kIID_IDirectInput2A)
        || GuidEquals(iid, &kIID_IDirectInput7A);
}

std::wstring AnsiToWide(const char* s)
{
    if (!s || !*s)
        return {};

    int len = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);

    if (len <= 1)
        return {};

    std::wstring out(static_cast<size_t>(len) - 1, L'\0');

    MultiByteToWideChar(CP_ACP, 0, s, -1, out.data(), len);

    return out;
}

// Patch a single vtable slot in place. Swapping the object's vtable POINTER
// instead makes DirectInput reject calls with E_INVALIDARG, because it
// identifies the interface by inspecting that pointer.
void PatchVTableSlot(void** vtable, int index, void* replacement, void** outOriginal)
{
    DWORD previousProtection = 0;

    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &previousProtection))
        return;

    if (outOriginal)
        *outOriginal = vtable[index];

    vtable[index] = replacement;

    VirtualProtect(&vtable[index], sizeof(void*), previousProtection, &previousProtection);
}

void** VTableOf(void* object)
{
    return object ? *static_cast<void***>(object) : nullptr;
}

// Hand back the same object, with a reference taken, for interfaces whose
// vtable layout the DX8 object already satisfies.
HRESULT AliasSelf(void* self, void** out)
{
    void** vtable = VTableOf(self);

    if (!vtable)
        return E_NOINTERFACE;

    *out = self;

    reinterpret_cast<PfnAddRef>(vtable[1])(self);

    return S_OK;
}

// ---------------------------------------------------------------------------
// Device filter
// ---------------------------------------------------------------------------

bool FilterActive()
{
    return g_diFilterEnabled && !(g_diFilterClasses.empty() && g_diFilterNames.empty());
}

bool ClassAllowed(DiDeviceClass deviceClass)
{
    for (DiDeviceClass allowed : g_diFilterClasses)
    {
        if (allowed == deviceClass)
            return true;
    }

    return false;
}

// Allow-list semantics, matching NetworkAdapters: a device is kept if it
// matches ANY populated list. `reason` receives a short explanation for the
// Debug log.
bool DeviceAllowed(DiDeviceClass deviceClass, const std::wstring& instanceName,
    const std::wstring& productName, std::wstring& reason)
{
    if (!FilterActive())
        return true;

    if (ClassAllowed(deviceClass))
    {
        reason = std::wstring(L"class=") + DiClassName(deviceClass);
        return true;
    }

    for (const DiNameFilter& rule : g_diFilterNames)
    {
        if (std::regex_search(instanceName, rule.pattern) ||
            std::regex_search(productName, rule.pattern))
        {
            reason = L"name matched " + rule.patternText;
            return true;
        }

        if (g_logLevel >= LogLevel::Trace)
            LogDirectInputDiag(L"DINPUT RULE", instanceName.c_str(),
                (L"no match: " + rule.patternText).c_str());
    }

    reason = L"no class or name rule matched";
    return false;
}

// True when the filter admits nothing beyond the system mouse and keyboard, in
// which case the enumeration can be answered directly instead of run. A real
// enumeration opens and classifies the whole HID stack — measured at ~21 s per
// call on a large one, whatever class was requested — and those two devices
// need no enumeration to be created. Any name rule could match an arbitrary
// device, so the fast path requires the class list alone.
bool CanAnswerWithoutEnumerating()
{
    if (!FilterActive() || !g_diFilterNames.empty() || g_diFilterClasses.empty())
        return false;

    for (DiDeviceClass allowed : g_diFilterClasses)
    {
        if (allowed != DiDeviceClass::Mouse && allowed != DiDeviceClass::Keyboard)
            return false;
    }

    return true;
}

void LogDeviceFound(const std::wstring& instanceName, const std::wstring& productName,
    DiDeviceClass deviceClass, DWORD devType)
{
    if (!g_logDirectInput || g_logLevel < LogLevel::Debug)
        return;

    wchar_t detail[160]{};

    wsprintfW(detail, L"class=%s  type=0x%08X", DiClassName(deviceClass), devType);

    const std::wstring& label = instanceName.empty() ? productName : instanceName;

    LogDirectInputDiag(L"DINPUT FOUND", label.empty() ? L"(unnamed)" : label.c_str(), detail);
}

// ---------------------------------------------------------------------------
// Enumeration
//
// One detour serves both the bridged DirectInput 3/7 path and native
// DirectInput 8, in ANSI and Unicode flavours. The thunk sits between
// DirectInput and the caller's callback so every device can be logged and
// filtered on the way past.
// ---------------------------------------------------------------------------

struct EnumContext
{
    void* userCallback = nullptr;
    void* userRef      = nullptr;
    int   found        = 0;
    int   shown        = 0;
};

BOOL WINAPI EnumThunkA(const DiDeviceInstanceA* instance, void* ref)
{
    EnumContext* ctx = static_cast<EnumContext*>(ref);

    if (!instance)
        return TRUE;

    ++ctx->found;

    const DiDeviceClass deviceClass  = DiClassFromDevType(instance->dwDevType);
    const std::wstring  instanceName = AnsiToWide(instance->tszInstanceName);
    const std::wstring  productName  = AnsiToWide(instance->tszProductName);

    LogDeviceFound(instanceName, productName, deviceClass, instance->dwDevType);

    std::wstring reason;

    if (!DeviceAllowed(deviceClass, instanceName, productName, reason))
    {
        LogDirectInputDiag(L"DINPUT HIDDEN",
            instanceName.empty() ? L"(unnamed)" : instanceName.c_str(), reason.c_str());

        return TRUE; // DIENUM_CONTINUE — skip this device, keep enumerating
    }

    ++ctx->shown;

    return reinterpret_cast<PfnEnumCallbackA>(ctx->userCallback)(instance, ctx->userRef);
}

BOOL WINAPI EnumThunkW(const DiDeviceInstanceW* instance, void* ref)
{
    EnumContext* ctx = static_cast<EnumContext*>(ref);

    if (!instance)
        return TRUE;

    ++ctx->found;

    const DiDeviceClass deviceClass  = DiClassFromDevType(instance->dwDevType);
    const std::wstring  instanceName = instance->tszInstanceName;
    const std::wstring  productName  = instance->tszProductName;

    LogDeviceFound(instanceName, productName, deviceClass, instance->dwDevType);

    std::wstring reason;

    if (!DeviceAllowed(deviceClass, instanceName, productName, reason))
    {
        LogDirectInputDiag(L"DINPUT HIDDEN",
            instanceName.empty() ? L"(unnamed)" : instanceName.c_str(), reason.c_str());

        return TRUE;
    }

    ++ctx->shown;

    return reinterpret_cast<PfnEnumCallbackW>(ctx->userCallback)(instance, ctx->userRef);
}

void LogEnumSummary(const EnumContext& ctx)
{
    if (!g_logDirectInput)
        return;

    wchar_t detail[96]{};

    wsprintfW(detail, L"%d found, %d shown", ctx.found, ctx.shown);

    LogDirectInput(L"DINPUT ENUM", L"enumerated devices", detail);
}

// Hand the caller one DIDEVICEINSTANCEA without running an enumeration.
// Returns the callback's verdict; FALSE means DIENUM_STOP.
BOOL ReportDevice(PfnEnumCallbackA callback, void* ref, const GUID& guid, DWORD deviceType,
    const char* name)
{
    DiDeviceInstanceA instance{};

    instance.dwSize       = sizeof(instance);
    instance.guidInstance = guid;
    instance.guidProduct  = guid;
    instance.dwDevType    = deviceType;

    lstrcpynA(instance.tszInstanceName, name, MAX_PATH);
    lstrcpynA(instance.tszProductName,  name, MAX_PATH);

    return callback(&instance, ref);
}

// The fast path. Only ever taken for the ANSI interface: it exists to keep
// DirectInput 3/7 startup times sane, and those games are all ANSI.
HRESULT AnswerWithoutEnumerating(DWORD deviceClass, PfnEnumCallbackA callback, void* ref)
{
    EnumContext ctx;

    LogDirectInput(L"DINPUT ENUM", L"answered without enumerating",
        L"filter admits only the system mouse and keyboard");

    if ((deviceClass == kDevClassAll || deviceClass == kDevClassMouse) &&
        ClassAllowed(DiDeviceClass::Mouse))
    {
        ++ctx.found;
        ++ctx.shown;

        LogDeviceFound(L"Mouse", L"Mouse", DiDeviceClass::Mouse, kDevTypeMouseTraditional);

        if (!ReportDevice(callback, ref, kGUID_SysMouse, kDevTypeMouseTraditional, "Mouse"))
        {
            LogEnumSummary(ctx);
            return S_OK;
        }
    }

    if ((deviceClass == kDevClassAll || deviceClass == kDevClassKeyboard) &&
        ClassAllowed(DiDeviceClass::Keyboard))
    {
        ++ctx.found;
        ++ctx.shown;

        LogDeviceFound(L"Keyboard", L"Keyboard", DiDeviceClass::Keyboard, kDevTypeKeyboardPcEnh);

        if (!ReportDevice(callback, ref, kGUID_SysKeyboard, kDevTypeKeyboardPcEnh, "Keyboard"))
        {
            LogEnumSummary(ctx);
            return S_OK;
        }
    }

    LogEnumSummary(ctx);

    return S_OK;
}

HRESULT WINAPI EnumDevicesDetourA(void* self, DWORD deviceClass, PfnEnumCallbackA callback,
    void* ref, DWORD flags)
{
    if (!callback)
        return E_POINTER;

    if (CanAnswerWithoutEnumerating())
        return AnswerWithoutEnumerating(deviceClass, callback, ref);

    EnumContext ctx;
    ctx.userCallback = reinterpret_cast<void*>(callback);
    ctx.userRef      = ref;

    HRESULT hr = g_origEnumDevicesA(self, deviceClass, EnumThunkA, &ctx, flags);

    LogEnumSummary(ctx);

    return hr;
}

HRESULT WINAPI EnumDevicesDetourW(void* self, DWORD deviceClass, PfnEnumCallbackW callback,
    void* ref, DWORD flags)
{
    if (!callback)
        return E_POINTER;

    EnumContext ctx;
    ctx.userCallback = reinterpret_cast<void*>(callback);
    ctx.userRef      = ref;

    HRESULT hr = g_origEnumDevicesW(self, deviceClass, EnumThunkW, &ctx, flags);

    LogEnumSummary(ctx);

    return hr;
}

// Patch slot 4 of whichever IDirectInput8 vtable this object belongs to. Called
// for every object we create or see created; each vtable is patched once.
void AttachEnumFilter(void* directInput, bool unicode)
{
    void** vtable = VTableOf(directInput);

    if (!vtable)
        return;

    if (unicode)
    {
        if (g_enumVTableW)
            return;

        PatchVTableSlot(vtable, 4, reinterpret_cast<void*>(EnumDevicesDetourW),
            reinterpret_cast<void**>(&g_origEnumDevicesW));

        g_enumVTableW = vtable;
    }
    else
    {
        if (g_enumVTableA)
            return;

        PatchVTableSlot(vtable, 4, reinterpret_cast<void*>(EnumDevicesDetourA),
            reinterpret_cast<void**>(&g_origEnumDevicesA));

        g_enumVTableA = vtable;
    }
}

// ---------------------------------------------------------------------------
// Mouse transform plumbing
// ---------------------------------------------------------------------------

// DIDATAFORMAT, as far as we need it. The trailing pointer is why this is not
// a fixed table of offsets: rgodf is pointer-sized, so the struct differs
// between x86 and x64 and has to be described rather than assumed.
struct DiDataFormat
{
    DWORD       dwSize;
    DWORD       dwObjSize;
    DWORD       dwFlags;
    DWORD       dwDataSize;
    DWORD       dwNumObjs;
    const BYTE* rgodf;
};

// DIOBJECTDATAFORMAT — { const GUID* pguid; DWORD dwOfs; DWORD dwType; DWORD dwFlags; }
struct DiObjectDataFormat
{
    const GUID* pguid;
    DWORD       dwOfs;
    DWORD       dwType;
    DWORD       dwFlags;
};

// Learn which dwOfs the game gave each axis in its own data format. Without
// this a transform cannot tell an axis event from a button event, because the
// offsets are whatever the game decided.
void LearnAxisOffsets(const void* format)
{
    g_axisOffsetX = INTERPOSER_MOUSE_AXIS_NONE;
    g_axisOffsetY = INTERPOSER_MOUSE_AXIS_NONE;
    g_axisOffsetZ = INTERPOSER_MOUSE_AXIS_NONE;

    const DiDataFormat* df = static_cast<const DiDataFormat*>(format);

    if (!df || !df->rgodf || df->dwNumObjs == 0 || df->dwNumObjs > 256)
        return;

    // dwObjSize rather than sizeof(DiObjectDataFormat): the format declares its
    // own stride, and trusting ours would walk the array wrong if it differs.
    const DWORD stride = df->dwObjSize ? df->dwObjSize
                                       : static_cast<DWORD>(sizeof(DiObjectDataFormat));

    for (DWORD i = 0; i < df->dwNumObjs; ++i)
    {
        const DiObjectDataFormat* obj =
            reinterpret_cast<const DiObjectDataFormat*>(df->rgodf + static_cast<size_t>(i) * stride);

        if (!obj->pguid)
            continue;

        switch (obj->pguid->Data1)
        {
        case kGuidXAxisData1: g_axisOffsetX = obj->dwOfs; break;
        case kGuidYAxisData1: g_axisOffsetY = obj->dwOfs; break;
        case kGuidZAxisData1: g_axisOffsetZ = obj->dwOfs; break;
        default: break;
        }
    }

    if (g_logDirectInput && g_logLevel >= LogLevel::Debug)
    {
        wchar_t detail[128]{};

        wsprintfW(detail, L"X=%d  Y=%d  Z=%d  objects=%u",
            static_cast<int>(g_axisOffsetX), static_cast<int>(g_axisOffsetY),
            static_cast<int>(g_axisOffsetZ), df->dwNumObjs);

        LogDirectInputDiag(L"DINPUT FORMAT", L"mouse axis offsets", detail);
    }
}

void QueuePending(const InterposerInputEvent* events, DWORD count)
{
    if (count > kPendingMax)
        count = kPendingMax;

    memcpy(g_pending, events, count * sizeof(InterposerInputEvent));

    g_pendingCount = count;
}

// Move up to `want` queued events into the caller's DX3 buffer.
DWORD DrainPending(DiObjectDataDx3* out, DWORD want)
{
    DWORD moved = 0;

    while (g_pendingCount > 0 && moved < want)
    {
        out[moved].dwOfs       = g_pending[0].dwOfs;
        out[moved].dwData      = static_cast<DWORD>(g_pending[0].data);
        out[moved].dwTimeStamp = g_pending[0].timeStamp;
        out[moved].dwSequence  = g_pending[0].sequence;

        --g_pendingCount;
        memmove(g_pending, g_pending + 1, g_pendingCount * sizeof(InterposerInputEvent));

        ++moved;
    }

    return moved;
}

// ---------------------------------------------------------------------------
// Device interface detours (bridged games only)
// ---------------------------------------------------------------------------

// Nothing but a way to see the game's data format go past — the offsets it
// declares are what make a transform able to identify the axes.
HRESULT WINAPI DeviceSetDataFormat(void* self, const void* format)
{
    HRESULT hr = g_origSetDataFormat(self, format);

    if (SUCCEEDED(hr) && format && self == g_mouseDevice)
    {
        LearnAxisOffsets(format);

        // A fresh format means the previous queue describes offsets that no
        // longer mean anything.
        g_pendingCount = 0;
    }

    return hr;
}

HRESULT WINAPI DeviceQueryInterface(void* self, const GUID* iid, void** out)
{
    if (iid && out &&
        (GuidEquals(iid, &kIID_IDirectInputDeviceA)  ||
         GuidEquals(iid, &kIID_IDirectInputDevice2A) ||
         GuidEquals(iid, &kIID_IDirectInputDevice7A)))
    {
        LogDirectInput(L"DINPUT DEVICE", L"QueryInterface", L"aliased pre-DX8 device IID");

        return AliasSelf(self, out);
    }

    return g_origDeviceQI(self, iid, out);
}

// DIDEVICEOBJECTDATA gained uAppData in DX8. A pre-DX8 game passes the DX3
// layout, and DirectInput 8 rejects that outright — passing cbObjectData = 16
// straight through returns E_INVALIDARG every time (measured: 6907 calls, zero
// successes). So the records have to be translated.
HRESULT WINAPI DeviceGetDeviceData(void* self, DWORD cbObjectData, void* rgdod,
    DWORD* pdwInOut, DWORD flags)
{
    // Anything that is not the DX3 layout is already what DirectInput 8 wants.
    if (cbObjectData != sizeof(DiObjectDataDx3))
        return g_origGetDeviceData(self, cbObjectData, rgdod, pdwInOut, flags);

    // A null buffer means "count" or "flush the queue" — no records to convert.
    if (!rgdod)
        return g_origGetDeviceData(self, sizeof(DiObjectDataDx8), nullptr, pdwInOut, flags);

    if (!pdwInOut)
        return E_POINTER;

    if (*pdwInOut == 0)
        return g_origGetDeviceData(self, sizeof(DiObjectDataDx8), nullptr, pdwInOut, flags);

    const DWORD want = *pdwInOut;
    const bool  peek = (flags & 1) != 0; // DIGDD_PEEK — must not consume anything

    // Mouse transform path. DirectInput hands the game a queue of many tiny
    // axis events and games of this era drain it one per frame, so a transform
    // only has something to work with if a whole batch is pulled at once. What
    // the game does not take is held for the next call rather than dropped —
    // events pulled and not returned are gone from DirectInput's buffer.
    if (g_mouseTransform && !peek && self == g_mouseDevice)
    {
        DiObjectDataDx3* out     = static_cast<DiObjectDataDx3*>(rgdod);
        DWORD            written = DrainPending(out, want);

        if (written < want)
        {
            DiObjectDataDx8 scratch[kEventBatch];
            DWORD           retrieved = kEventBatch;

            HRESULT hr = g_origGetDeviceData(self, sizeof(DiObjectDataDx8), scratch, &retrieved, flags);

            if (FAILED(hr))
            {
                *pdwInOut = written;
                return written > 0 ? S_OK : hr;
            }

            if (retrieved > kEventBatch)
                retrieved = kEventBatch;

            InterposerInputEvent events[kPendingMax];

            for (DWORD i = 0; i < retrieved; ++i)
            {
                events[i].dwOfs     = scratch[i].dwOfs;
                events[i].data      = static_cast<LONG>(scratch[i].dwData);
                events[i].timeStamp = scratch[i].dwTimeStamp;
                events[i].sequence  = scratch[i].dwSequence;
            }

            InterposerMouseBatch batch{};
            batch.structSize  = sizeof(batch);
            batch.axisOffsetX = g_axisOffsetX;
            batch.axisOffsetY = g_axisOffsetY;
            batch.axisOffsetZ = g_axisOffsetZ;
            batch.events      = events;
            batch.count       = retrieved;
            batch.capacity    = kPendingMax;

            g_mouseTransform(&batch, g_mouseTransformUser);

            QueuePending(events, batch.count);

            written += DrainPending(out + written, want - written);
        }

        *pdwInOut = written;
        return S_OK;
    }

    // Never pull more events than can be handed back: anything pulled and not
    // returned is gone from DirectInput's buffer, and games on this path read
    // one event at a time. Returning fewer records than asked for is allowed.
    DWORD requested = want < kEventBatch ? want : kEventBatch;
    DWORD retrieved = requested;

    DiObjectDataDx8 scratch[kEventBatch];

    HRESULT hr = g_origGetDeviceData(self, sizeof(DiObjectDataDx8), scratch, &retrieved, flags);

    if (FAILED(hr))
    {
        *pdwInOut = 0;
        return hr;
    }

    if (retrieved > requested)
        retrieved = requested;

    DiObjectDataDx3* out = static_cast<DiObjectDataDx3*>(rgdod);

    for (DWORD i = 0; i < retrieved; ++i)
    {
        out[i].dwOfs       = scratch[i].dwOfs;
        out[i].dwData      = scratch[i].dwData;
        out[i].dwTimeStamp = scratch[i].dwTimeStamp;
        out[i].dwSequence  = scratch[i].dwSequence;
    }

    *pdwInOut = retrieved;

    // hr is preserved rather than flattened to S_OK so DI_BUFFEROVERFLOW still
    // reaches the game.
    return hr;
}

// ---------------------------------------------------------------------------
// IDirectInput detours (bridged games only)
// ---------------------------------------------------------------------------

HRESULT WINAPI DirectInputQueryInterface(void* self, const GUID* iid, void** out)
{
    if (iid && out && IsAnsiDirectInputIid(iid))
    {
        LogDirectInput(L"DINPUT BRIDGE", L"QueryInterface", L"aliased pre-DX8 IDirectInput IID");

        return AliasSelf(self, out);
    }

    return g_origDirectInputQI(self, iid, out);
}

HRESULT WINAPI DirectInputCreateDevice(void* self, const GUID* rguid, void** ppDevice,
    void* punkOuter)
{
    HRESULT hr = g_origCreateDevice(self, rguid, ppDevice, punkOuter);

    if (FAILED(hr) || !ppDevice || !*ppDevice)
        return hr;

    LogDirectInput(L"DINPUT DEVICE", L"CreateDevice",
        GuidEquals(rguid, &kGUID_SysMouse)    ? L"GUID_SysMouse"    :
        GuidEquals(rguid, &kGUID_SysKeyboard) ? L"GUID_SysKeyboard" : L"other");

    // Remembered so the device detours can tell the mouse from the keyboard,
    // which share this vtable. Re-creating it invalidates the learned format.
    if (GuidEquals(rguid, &kGUID_SysMouse))
    {
        g_mouseDevice  = *ppDevice;
        g_pendingCount = 0;
        g_axisOffsetX  = INTERPOSER_MOUSE_AXIS_NONE;
        g_axisOffsetY  = INTERPOSER_MOUSE_AXIS_NONE;
        g_axisOffsetZ  = INTERPOSER_MOUSE_AXIS_NONE;
    }

    // Mouse and keyboard share one device vtable, so this runs exactly once.
    if (!g_deviceVTable)
    {
        if (void** vtable = VTableOf(*ppDevice))
        {
            PatchVTableSlot(vtable,  0, reinterpret_cast<void*>(DeviceQueryInterface),
                reinterpret_cast<void**>(&g_origDeviceQI));
            PatchVTableSlot(vtable, 10, reinterpret_cast<void*>(DeviceGetDeviceData),
                reinterpret_cast<void**>(&g_origGetDeviceData));
            PatchVTableSlot(vtable, 11, reinterpret_cast<void*>(DeviceSetDataFormat),
                reinterpret_cast<void**>(&g_origSetDataFormat));

            g_deviceVTable = vtable;
        }
    }

    return hr;
}

// ---------------------------------------------------------------------------
// dinput8.dll
// ---------------------------------------------------------------------------

bool LoadDirectInput8()
{
    if (g_di8Create)
        return true;

    wchar_t path[MAX_PATH]{};

    UINT length = GetSystemDirectoryW(path, MAX_PATH);

    if (length == 0 || length > MAX_PATH - 16)
        return false;

    wcscat_s(path, L"\\dinput8.dll");

    g_di8 = LoadLibraryW(path);

    if (!g_di8)
    {
        LogDirectInput(L"DINPUT BRIDGE", L"failed to load", path);
        return false;
    }

    g_di8Create = reinterpret_cast<PfnDirectInput8Create>(
        reinterpret_cast<void*>(GetProcAddress(g_di8, "DirectInput8Create")));

    if (!g_di8Create)
        LogDirectInput(L"DINPUT BRIDGE", L"dinput8.dll has no DirectInput8Create");

    return g_di8Create != nullptr;
}

// Reach DirectInput8Create without re-entering our own detour. When the export
// is hooked, GetProcAddress hands back a patched prologue, so the MinHook
// trampoline has to be preferred or creating an object would recurse.
PfnDirectInput8Create RealDirectInput8Create()
{
    // Null whenever the export was not hooked, which includes the case where
    // this binary IS dinput8.dll — then GetProcAddress on the system copy is
    // both correct and the only way to avoid calling our own export.
    if (g_origDirectInput8Create)
        return g_origDirectInput8Create;

    return LoadDirectInput8() ? g_di8Create : nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

BOOL InterposerRegisterMouseTransform(InterposerMouseTransform callback, void* userData)
{
    g_mouseTransform     = callback;
    g_mouseTransformUser = userData;
    g_pendingCount       = 0;

    LogDirectInput(L"DINPUT", callback ? L"mouse transform registered"
                                       : L"mouse transform cleared");

    return TRUE;
}

bool DirectInputBridgeWantsIid(const void* riid)
{
    return IsAnsiDirectInputIid(static_cast<const GUID*>(riid));
}

HRESULT DirectInput8CreateFiltered(HINSTANCE hinst, DWORD version, const void* riid,
    void** ppvOut, void* punkOuter)
{
    PfnDirectInput8Create create = RealDirectInput8Create();

    if (!create)
        return E_FAIL;

    const GUID* iid = static_cast<const GUID*>(riid);

    HRESULT hr = create(hinst, version, iid, ppvOut, punkOuter);

    if (SUCCEEDED(hr) && ppvOut && *ppvOut)
        AttachEnumFilter(*ppvOut, GuidEquals(iid, &kIID_IDirectInput8W));

    return hr;
}

HRESULT DirectInputBridgeCreate(HINSTANCE hinst, DWORD version, void** ppDI, void* punkOuter)
{
    // The requested version is deliberately ignored — 0x0300 and 0x0700 both
    // land on the legacy code path this exists to avoid.
    UNREFERENCED_PARAMETER(version);

    if (!ppDI)
        return E_POINTER;

    *ppDI = nullptr;

    PfnDirectInput8Create create = RealDirectInput8Create();

    if (!create)
        return E_FAIL;

    HRESULT hr = create(hinst, 0x0800, &kIID_IDirectInput8A, ppDI, punkOuter);

    if (FAILED(hr) || !*ppDI)
        return FAILED(hr) ? hr : E_FAIL;

    // IDirectInput7A and IDirectInput8A agree for slots 0..8, and the device
    // interfaces agree for slots 0..28, so the game can drive this object
    // directly once QueryInterface, CreateDevice and EnumDevices are bridged.
    void** vtable = VTableOf(*ppDI);

    if (!vtable)
        return E_FAIL;

    if (!g_bridgeVTable)
    {
        PatchVTableSlot(vtable, 0, reinterpret_cast<void*>(DirectInputQueryInterface),
            reinterpret_cast<void**>(&g_origDirectInputQI));
        PatchVTableSlot(vtable, 3, reinterpret_cast<void*>(DirectInputCreateDevice),
            reinterpret_cast<void**>(&g_origCreateDevice));

        g_bridgeVTable = vtable;
    }

    // Slot 4 is shared with native DirectInput 8 games, so it goes through the
    // same attach path rather than being patched separately here.
    AttachEnumFilter(*ppDI, false);

    LogDirectInput(L"DINPUT BRIDGE", L"created IDirectInput8A",
        CanAnswerWithoutEnumerating() ? L"enumeration answered from the filter"
                                      : L"real enumeration");

    return S_OK;
}

// ---------------------------------------------------------------------------
// Hook installation
// ---------------------------------------------------------------------------

namespace {

// True when the named module resolves to this one. This is the guard that makes
// a single binary safe to deploy under any name: when the file IS dinput.dll or
// dinput8.dll, its exports are served directly by proxy.cpp and hooking them
// would detour us straight back into ourselves.
bool ModuleIsSelf(const wchar_t* moduleName)
{
    HMODULE self = nullptr;

    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ModuleIsSelf), &self);

    return self && self == GetModuleHandleW(moduleName);
}

HRESULT WINAPI HookDirectInputCreateA(HINSTANCE hinst, DWORD version, void** ppDI, void* punkOuter)
{
    LogDirectInput(L"DINPUT", L"DirectInputCreateA");

    return DirectInputBridgeCreate(hinst, version, ppDI, punkOuter);
}

// The Unicode interfaces are not bridged — every game seen on this path uses
// the ANSI entry point, and bridging DIDEVICEINSTANCEW would be untested code.
// Unicode callers keep the behaviour they had without us.
HRESULT WINAPI HookDirectInputCreateEx(HINSTANCE hinst, DWORD version, const GUID* riid,
    void** ppvOut, void* punkOuter)
{
    if (!IsAnsiDirectInputIid(riid))
        return g_origDirectInputCreateEx(hinst, version, riid, ppvOut, punkOuter);

    LogDirectInput(L"DINPUT", L"DirectInputCreateEx");

    return DirectInputBridgeCreate(hinst, version, ppvOut, punkOuter);
}

void InstallLegacyCreateHooks()
{
    if (!g_diFixLegacyEnum || ModuleIsSelf(L"dinput.dll"))
        return;

    LogHookInit(L"dinput.dll", "DirectInputCreateA",
        MH_CreateHookApi(L"dinput.dll", "DirectInputCreateA",
            reinterpret_cast<LPVOID>(HookDirectInputCreateA),
            reinterpret_cast<LPVOID*>(&g_origDirectInputCreateA)));

    LogHookInit(L"dinput.dll", "DirectInputCreateEx",
        MH_CreateHookApi(L"dinput.dll", "DirectInputCreateEx",
            reinterpret_cast<LPVOID>(HookDirectInputCreateEx),
            reinterpret_cast<LPVOID*>(&g_origDirectInputCreateEx)));
}

HRESULT WINAPI HookDirectInput8Create(HINSTANCE hinst, DWORD version, const GUID* riid,
    void** ppvOut, void* punkOuter)
{
    HRESULT hr = g_origDirectInput8Create(hinst, version, riid, ppvOut, punkOuter);

    if (SUCCEEDED(hr) && ppvOut && *ppvOut)
    {
        LogDirectInput(L"DINPUT", L"DirectInput8Create");

        AttachEnumFilter(*ppvOut, GuidEquals(riid, &kIID_IDirectInput8W));
    }

    return hr;
}

// Wanted whenever there is something to do to a native DirectInput 8
// enumeration: filter it, or list what it found. Bridged games reach the same
// detour through DirectInputBridgeCreate and do not need this hook.
bool DirectInput8HookWanted()
{
    return FilterActive() || g_logDirectInput;
}

void InstallDirectInput8Hook()
{
    if (!DirectInput8HookWanted() || ModuleIsSelf(L"dinput8.dll"))
        return;

    LogHookInit(L"dinput8.dll", "DirectInput8Create",
        MH_CreateHookApi(L"dinput8.dll", "DirectInput8Create",
            reinterpret_cast<LPVOID>(HookDirectInput8Create),
            reinterpret_cast<LPVOID*>(&g_origDirectInput8Create)));
}

} // namespace

void InstallDirectInputHooks()
{
    // MH_ERROR_MODULE_NOT_FOUND here means the DLL was not loaded at inject
    // time; OnLibraryLoaded retries via LateInstallDirectInputHooks.
    InstallLegacyCreateHooks();
    InstallDirectInput8Hook();
}

void LateInstallDirectInputHooks(const wchar_t* moduleName)
{
    if (_wcsicmp(moduleName, L"dinput.dll") == 0)
    {
        InstallLegacyCreateHooks();
        MH_EnableHook(MH_ALL_HOOKS);
        return;
    }

    if (_wcsicmp(moduleName, L"dinput8.dll") == 0)
    {
        InstallDirectInput8Hook();
        MH_EnableHook(MH_ALL_HOOKS);
    }
}

void RemoveDirectInputHooks()
{
    // Put the vtable slots back. MinHook restores its own trampolines but knows
    // nothing about these, and a dinput8 vtable still pointing into this DLL
    // after it unloads would fault on the game's next call.
    if (g_deviceVTable)
    {
        if (g_origDeviceQI)
            PatchVTableSlot(g_deviceVTable, 0, reinterpret_cast<void*>(g_origDeviceQI), nullptr);
        if (g_origGetDeviceData)
            PatchVTableSlot(g_deviceVTable, 10, reinterpret_cast<void*>(g_origGetDeviceData), nullptr);
        if (g_origSetDataFormat)
            PatchVTableSlot(g_deviceVTable, 11, reinterpret_cast<void*>(g_origSetDataFormat), nullptr);

        g_deviceVTable      = nullptr;
        g_origDeviceQI      = nullptr;
        g_origGetDeviceData = nullptr;
        g_origSetDataFormat = nullptr;
    }

    // The transform lives in a plugin, which is unloaded around the same time.
    g_mouseTransform     = nullptr;
    g_mouseTransformUser = nullptr;
    g_mouseDevice        = nullptr;
    g_pendingCount       = 0;

    if (g_bridgeVTable)
    {
        if (g_origDirectInputQI)
            PatchVTableSlot(g_bridgeVTable, 0, reinterpret_cast<void*>(g_origDirectInputQI), nullptr);
        if (g_origCreateDevice)
            PatchVTableSlot(g_bridgeVTable, 3, reinterpret_cast<void*>(g_origCreateDevice), nullptr);

        g_bridgeVTable      = nullptr;
        g_origDirectInputQI = nullptr;
        g_origCreateDevice  = nullptr;
    }

    if (g_enumVTableA)
    {
        if (g_origEnumDevicesA)
            PatchVTableSlot(g_enumVTableA, 4, reinterpret_cast<void*>(g_origEnumDevicesA), nullptr);

        g_enumVTableA      = nullptr;
        g_origEnumDevicesA = nullptr;
    }

    if (g_enumVTableW)
    {
        if (g_origEnumDevicesW)
            PatchVTableSlot(g_enumVTableW, 4, reinterpret_cast<void*>(g_origEnumDevicesW), nullptr);

        g_enumVTableW      = nullptr;
        g_origEnumDevicesW = nullptr;
    }

    // dinput8.dll is deliberately NOT freed. The game may still be holding an
    // object at process detach, and unloading dinput8 out from under it would
    // fault on the next call. The reference dies with the process.
    g_di8Create = nullptr;
    g_di8       = nullptr;
}
