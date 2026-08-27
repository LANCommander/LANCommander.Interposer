//
// LANCommander.Interposer.Plugin.Mouse
//
// Smooths and scales the buffered mouse stream on its way from DirectInput to
// the game. Ported from the AvP2 dinput shim, where both halves were needed to
// make the game playable.
//
// COALESCING
//
//   DirectInput buffered mode hands the game a queue of many small axis events.
//   Engines of this era drain it with GetDeviceData(want = 1) and apply one per
//   frame, so at a modern polling rate the view stair-steps: a 1000 Hz mouse
//   produces roughly sixteen events per frame at 60 fps and the game consumes
//   one of them. Summing the X events into a single event, and the Y events
//   into a single event, delivers the same total movement in one step instead
//   of sixteen. The game accumulates deltas anyway, so the sum is equivalent —
//   it just arrives when it happened rather than over the next quarter second.
//
//   Button and wheel events are passed through untouched and in order.
//
// SCALING
//
//   Some engines have an axis ratio that cannot be reached from their config.
//   A fractional remainder is carried per axis so slow movement survives: with
//   a 1.82 multiplier a raw delta of 1 becomes 2, not 1.
//
// Config.yml entries (under Plugins.Mouse):
//
//   Plugins:
//     Mouse:
//       CoalesceAxisEvents: 'true'   # sum each axis into one event per read
//       XMultiplier: '1.00'          # 1.00 leaves the axis alone
//       YMultiplier: '1.00'          # AvP2 wants 1.82 here
//
// Requires DirectInput.FixLegacyDeviceEnumeration to be enabled.
//
// WIN32_LEAN_AND_MEAN and NOMINMAX come from the project's preprocessor
// definitions; defining them here again only produces C4005.
#include <windows.h>

// ─── Interposer plugin API ────────────────────────────────────────────────────
// Declared here rather than included: the plugin resolves everything at runtime
// via GetProcAddress and has no link-time dependency on the Interposer.

typedef struct InterposerInputEvent {
    DWORD dwOfs;
    LONG  data;
    DWORD timeStamp;
    DWORD sequence;
} InterposerInputEvent;

#define INTERPOSER_MOUSE_AXIS_NONE 0xFFFFFFFFu

typedef struct InterposerMouseBatch {
    DWORD                 structSize;
    DWORD                 axisOffsetX;
    DWORD                 axisOffsetY;
    DWORD                 axisOffsetZ;
    InterposerInputEvent* events;
    DWORD                 count;
    DWORD                 capacity;
} InterposerMouseBatch;

// The Interposer's exports are __cdecl — they are declared without a calling
// convention and the export table carries their undecorated names. Declaring
// these pointers __stdcall instead leaks the arguments on every call on x86.
using FnMouseTransform = void (WINAPI*)(InterposerMouseBatch* batch, void* userData);

using FnInterposerLog                  = void (*)(const wchar_t* verb, const wchar_t* message);
using FnInterposerGetConfigString      = BOOL (*)(const wchar_t* dotPath, wchar_t* buf, DWORD bufSize);
using FnInterposerRegisterPluginConfig = BOOL (*)(const wchar_t* pluginName, const wchar_t* yamlDefaults);
using FnInterposerRegisterMouseTransform = BOOL (*)(FnMouseTransform callback, void* userData);

static FnInterposerLog                    pfnLog        = nullptr;
static FnInterposerGetConfigString        pfnGetConfig  = nullptr;
static FnInterposerRegisterPluginConfig   pfnRegConfig  = nullptr;
static FnInterposerRegisterMouseTransform pfnRegisterMouseTransform = nullptr;

static constexpr const wchar_t* kVerb = L"MOUSE";

// ─── Settings ─────────────────────────────────────────────────────────────────

static bool   g_coalesce    = true;
static double g_multiplierX = 1.0;
static double g_multiplierY = 1.0;

// Carried between calls so a scaled-down delta is not repeatedly truncated to
// zero. Only ever touched on the game's input thread.
static double g_remainderX = 0.0;
static double g_remainderY = 0.0;

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Deliberately hand-rolled rather than wcstod: the CRT honours the process
// locale, and a game that has set a comma-decimal locale would otherwise parse
// '1.82' as 1. Accepts an optional sign and a single decimal point.
static double ParseNumber(const wchar_t* text, double fallback)
{
    if (!text || !*text)
        return fallback;

    const wchar_t* p = text;

    while (*p == L' ' || *p == L'\t')
        ++p;

    bool negative = false;

    if (*p == L'-') { negative = true; ++p; }
    else if (*p == L'+') { ++p; }

    if (*p < L'0' || *p > L'9')
        if (*p != L'.')
            return fallback;

    double value = 0.0;

    while (*p >= L'0' && *p <= L'9')
    {
        value = value * 10.0 + static_cast<double>(*p - L'0');
        ++p;
    }

    if (*p == L'.')
    {
        ++p;

        double scale = 1.0;

        while (*p >= L'0' && *p <= L'9')
        {
            scale *= 10.0;
            value += static_cast<double>(*p - L'0') / scale;
            ++p;
        }
    }

    if (negative)
        value = -value;

    // A zero or absurd multiplier would silently disable the mouse.
    if (value <= 0.0 || value > 100.0)
        return fallback;

    return value;
}

static bool ParseBool(const wchar_t* text, bool fallback)
{
    if (!text || !*text)
        return fallback;

    if (_wcsicmp(text, L"true") == 0 || _wcsicmp(text, L"1") == 0 ||
        _wcsicmp(text, L"yes")  == 0 || _wcsicmp(text, L"on") == 0)
        return true;

    if (_wcsicmp(text, L"false") == 0 || _wcsicmp(text, L"0") == 0 ||
        _wcsicmp(text, L"no")    == 0 || _wcsicmp(text, L"off") == 0)
        return false;

    return fallback;
}

// Scale a delta, carrying the fraction so slow movement is not truncated away.
static LONG ScaleAxis(LONG value, double multiplier, double* remainder)
{
    const double scaled = static_cast<double>(value) * multiplier + *remainder;
    const LONG   whole  = static_cast<LONG>(scaled); // truncates toward zero

    *remainder = scaled - static_cast<double>(whole);

    return whole;
}

// ─── The transform ────────────────────────────────────────────────────────────

static void WINAPI MouseTransform(InterposerMouseBatch* batch, void* /*userData*/)
{
    if (!batch || batch->structSize < sizeof(InterposerMouseBatch) || !batch->events)
        return;

    const DWORD ofsX = batch->axisOffsetX;
    const DWORD ofsY = batch->axisOffsetY;

    const bool haveAxes = (ofsX != INTERPOSER_MOUSE_AXIS_NONE) ||
                          (ofsY != INTERPOSER_MOUSE_AXIS_NONE);

    if (!haveAxes)
        return; // nothing identifiable to act on; pass the batch through

    const bool scaling = (g_multiplierX != 1.0) || (g_multiplierY != 1.0);

    if (!g_coalesce && !scaling)
        return;

    // Without coalescing, scale each axis event where it sits and keep the
    // stream otherwise identical.
    if (!g_coalesce)
    {
        for (DWORD i = 0; i < batch->count; ++i)
        {
            InterposerInputEvent& e = batch->events[i];

            if (ofsX != INTERPOSER_MOUSE_AXIS_NONE && e.dwOfs == ofsX)
                e.data = ScaleAxis(e.data, g_multiplierX, &g_remainderX);
            else if (ofsY != INTERPOSER_MOUSE_AXIS_NONE && e.dwOfs == ofsY)
                e.data = ScaleAxis(e.data, g_multiplierY, &g_remainderY);
        }

        return;
    }

    // Coalesce: everything that is not an axis keeps its place and its order,
    // and the summed axes are appended once at the end.
    LONG  sumX = 0, sumY = 0;
    bool  haveX = false, haveY = false;
    DWORD stampX = 0, sequenceX = 0;
    DWORD stampY = 0, sequenceY = 0;
    DWORD kept = 0;

    for (DWORD i = 0; i < batch->count; ++i)
    {
        const InterposerInputEvent e = batch->events[i];

        if (ofsX != INTERPOSER_MOUSE_AXIS_NONE && e.dwOfs == ofsX)
        {
            sumX += e.data;
            haveX = true;
            stampX = e.timeStamp;
            sequenceX = e.sequence;
        }
        else if (ofsY != INTERPOSER_MOUSE_AXIS_NONE && e.dwOfs == ofsY)
        {
            sumY += e.data;
            haveY = true;
            stampY = e.timeStamp;
            sequenceY = e.sequence;
        }
        else
        {
            batch->events[kept++] = e;
        }
    }

    if (haveX && kept < batch->capacity)
    {
        const LONG scaled = ScaleAxis(sumX, g_multiplierX, &g_remainderX);

        if (scaled != 0)
        {
            batch->events[kept].dwOfs     = ofsX;
            batch->events[kept].data      = scaled;
            batch->events[kept].timeStamp = stampX;
            batch->events[kept].sequence  = sequenceX;
            ++kept;
        }
    }

    if (haveY && kept < batch->capacity)
    {
        const LONG scaled = ScaleAxis(sumY, g_multiplierY, &g_remainderY);

        if (scaled != 0)
        {
            batch->events[kept].dwOfs     = ofsY;
            batch->events[kept].data      = scaled;
            batch->events[kept].timeStamp = stampY;
            batch->events[kept].sequence  = sequenceY;
            ++kept;
        }
    }

    batch->count = kept;
}

// ─── Resolve interposer exports ───────────────────────────────────────────────

static bool ResolveAPI(HMODULE hInterposer)
{
    if (!hInterposer)
        return false;

    pfnLog       = reinterpret_cast<FnInterposerLog>(
        reinterpret_cast<void*>(GetProcAddress(hInterposer, "InterposerLog")));
    pfnGetConfig = reinterpret_cast<FnInterposerGetConfigString>(
        reinterpret_cast<void*>(GetProcAddress(hInterposer, "InterposerGetConfigString")));
    pfnRegConfig = reinterpret_cast<FnInterposerRegisterPluginConfig>(
        reinterpret_cast<void*>(GetProcAddress(hInterposer, "InterposerRegisterPluginConfig")));
    pfnRegisterMouseTransform = reinterpret_cast<FnInterposerRegisterMouseTransform>(
        reinterpret_cast<void*>(GetProcAddress(hInterposer, "InterposerRegisterMouseTransform")));

    return pfnLog && pfnGetConfig && pfnRegisterMouseTransform;
}

// ─── Plugin entry point ───────────────────────────────────────────────────────

extern "C" __declspec(dllexport) void WINAPI InterposerPluginInit(HMODULE hInterposer)
{
    if (!ResolveAPI(hInterposer))
        return; // exports unavailable — an older Interposer, or none at all

    if (pfnRegConfig)
    {
        pfnRegConfig(L"Mouse",
            L"CoalesceAxisEvents: 'true'\n"
            L"XMultiplier: '1.00'\n"
            L"YMultiplier: '1.00'");
    }

    wchar_t buffer[64] = {};

    if (pfnGetConfig(L"Plugins.Mouse.CoalesceAxisEvents", buffer, ARRAYSIZE(buffer)))
        g_coalesce = ParseBool(buffer, true);

    if (pfnGetConfig(L"Plugins.Mouse.XMultiplier", buffer, ARRAYSIZE(buffer)))
        g_multiplierX = ParseNumber(buffer, 1.0);

    if (pfnGetConfig(L"Plugins.Mouse.YMultiplier", buffer, ARRAYSIZE(buffer)))
        g_multiplierY = ParseNumber(buffer, 1.0);

    // Nothing configured to do — leave the input path completely alone rather
    // than registering a transform that would still force batched reads.
    if (!g_coalesce && g_multiplierX == 1.0 && g_multiplierY == 1.0)
    {
        pfnLog(kVerb, L"Coalescing off and both multipliers 1.00. Not registering");
        return;
    }

    if (!pfnRegisterMouseTransform(MouseTransform, nullptr))
    {
        pfnLog(kVerb, L"The Interposer refused the mouse transform");
        return;
    }

    wchar_t message[192];

    wsprintfW(message, L"coalesce=%s  X=%d.%02d  Y=%d.%02d",
        g_coalesce ? L"on" : L"off",
        static_cast<int>(g_multiplierX),
        static_cast<int>((g_multiplierX - static_cast<int>(g_multiplierX)) * 100.0 + 0.5),
        static_cast<int>(g_multiplierY),
        static_cast<int>((g_multiplierY - static_cast<int>(g_multiplierY)) * 100.0 + 0.5));

    pfnLog(kVerb, message);
}

BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD /*fdwReason*/, LPVOID /*lpReserved*/)
{
    return TRUE;
}
