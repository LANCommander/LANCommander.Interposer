#include "interposer_features.h"
#include "config.h"
#include "instance.h"

namespace
{
    struct FeatureName
    {
        DWORD          flag;
        const wchar_t* name;
    };

    // The names used in the log and in the CMake options (INTERPOSER_FEATURE_<NAME>).
    constexpr FeatureName kFeatureNames[] = {
        { INTERPOSER_FEATURE_FLAG_REGISTRY,      L"Registry" },
        { INTERPOSER_FEATURE_FLAG_FILES,         L"Files" },
        { INTERPOSER_FEATURE_FLAG_FASTDL,        L"FastDL" },
        { INTERPOSER_FEATURE_FLAG_DIRECTINPUT,   L"DirectInput" },
        { INTERPOSER_FEATURE_FLAG_AUDIO,         L"Audio" },
        { INTERPOSER_FEATURE_FLAG_OSVERSION,     L"OsVersion" },
        { INTERPOSER_FEATURE_FLAG_IDENTITY,      L"Identity" },
        { INTERPOSER_FEATURE_FLAG_NETWORK,       L"Network" },
        { INTERPOSER_FEATURE_FLAG_RELAY,         L"Relay" },
        { INTERPOSER_FEATURE_FLAG_RICH_PRESENCE, L"RichPresence" },
        { INTERPOSER_FEATURE_FLAG_MATCHES,       L"Matches" },
        { INTERPOSER_FEATURE_FLAG_VOICE,         L"Voice" },
        { INTERPOSER_FEATURE_FLAG_PLUGINS,       L"Plugins" },
    };

    // Every feature this platform implements, whether or not this image carries
    // it: what a full build reports.
    constexpr DWORD AllPlatformFeatures()
    {
        DWORD mask = 0;
        for (const FeatureName& feature : kFeatureNames)
            mask |= feature.flag;
        return mask & kPlatformFeatures;
    }
}

std::wstring FeatureNames(DWORD mask)
{
    std::wstring names;

    for (const FeatureName& feature : kFeatureNames)
    {
        if (!(mask & feature.flag))
            continue;

        if (!names.empty())
            names += L", ";

        names += feature.name;
    }

    return names;
}

void ReportFeaturesMissingFromPrimary(void (*diag)(const wchar_t* message))
{
    const platform::Module primary = PrimaryInstance();

    if (!primary)
        return;

    using GetFeaturesFn = DWORD (INTERPOSER_CALL*)(void);
    const auto getFeatures = reinterpret_cast<GetFeaturesFn>(platform::FindSymbol(primary, "InterposerGetFeatures"));

    // No export: an Interposer from before feature switches, which carried them all.
    if (!getFeatures)
        return;

    const DWORD missing = kCompiledFeatures & ~getFeatures();

    if (!missing)
        return;

    const std::wstring message =
        L"the Interposer that owns this process was built without features this copy carries, "
        L"so they are off: " + FeatureNames(missing);

    if (diag)
        diag(message.c_str());

    using LogFn = void (INTERPOSER_CALL*)(const wchar_t*, const wchar_t*);

    if (const auto log = reinterpret_cast<LogFn>(platform::FindSymbol(primary, "InterposerLog")))
        log(L"INSTANCE", message.c_str());
}

void LogCompiledFeatures()
{
    if (g_logLevel < LogLevel::Debug || kCompiledFeatures == AllPlatformFeatures())
        return;

    InterposerLog(L"INSTANCE", (L"built with: " + FeatureNames(kCompiledFeatures)).c_str());
    InterposerLog(L"INSTANCE", (L"built without: " + FeatureNames(AllPlatformFeatures() & ~kCompiledFeatures)).c_str());
}

extern "C" INTERPOSER_API DWORD InterposerGetFeatures(void)
{
    // Not FORWARD_TO_PRIMARY: a primary from before this export falls through
    // to here, and it carried every feature, which is not what this copy has.
    if (const platform::Module primary = PrimaryInstance())
    {
        using GetFeaturesFn = DWORD (INTERPOSER_CALL*)(void);
        static const auto forward = reinterpret_cast<GetFeaturesFn>(platform::FindSymbol(primary, "InterposerGetFeatures"));

        return forward ? forward() : AllPlatformFeatures();
    }

    return kCompiledFeatures;
}
