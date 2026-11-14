#pragma once

// Compile-time feature switches.
//
// The shipped DLL and .so build every feature, and get no definitions at all:
// each switch below defaults to 1. Only the embeddable static library is ever
// configured otherwise, through the INTERPOSER_FEATURE_<NAME> CMake options
// (build/Features.cmake), which pass INTERPOSER_FEATURE_<NAME>=0 for a feature
// left out. A feature that is out keeps its exports -- core/stubs/<name>_off.cpp
// answers each the way a build without the subsystem would -- so the API, the
// plugins that look it up by name and FORWARD_TO_PRIMARY all keep working.

#include "LANCommander.Interposer.h"

#ifndef INTERPOSER_FEATURE_REGISTRY
#define INTERPOSER_FEATURE_REGISTRY 1
#endif
#ifndef INTERPOSER_FEATURE_FILES
#define INTERPOSER_FEATURE_FILES 1
#endif
#ifndef INTERPOSER_FEATURE_FASTDL
#define INTERPOSER_FEATURE_FASTDL 1
#endif
#ifndef INTERPOSER_FEATURE_DIRECTINPUT
#define INTERPOSER_FEATURE_DIRECTINPUT 1
#endif
#ifndef INTERPOSER_FEATURE_AUDIO
#define INTERPOSER_FEATURE_AUDIO 1
#endif
#ifndef INTERPOSER_FEATURE_OSVERSION
#define INTERPOSER_FEATURE_OSVERSION 1
#endif
#ifndef INTERPOSER_FEATURE_IDENTITY
#define INTERPOSER_FEATURE_IDENTITY 1
#endif
#ifndef INTERPOSER_FEATURE_NETWORK
#define INTERPOSER_FEATURE_NETWORK 1
#endif
#ifndef INTERPOSER_FEATURE_RELAY
#define INTERPOSER_FEATURE_RELAY 1
#endif
#ifndef INTERPOSER_FEATURE_RICH_PRESENCE
#define INTERPOSER_FEATURE_RICH_PRESENCE 1
#endif
#ifndef INTERPOSER_FEATURE_MATCHES
#define INTERPOSER_FEATURE_MATCHES 1
#endif
#ifndef INTERPOSER_FEATURE_VOICE
#define INTERPOSER_FEATURE_VOICE 1
#endif
#ifndef INTERPOSER_FEATURE_PLUGINS
#define INTERPOSER_FEATURE_PLUGINS 1
#endif

#if INTERPOSER_FEATURE_FASTDL && !INTERPOSER_FEATURE_FILES
#error "INTERPOSER_FEATURE_FASTDL requires INTERPOSER_FEATURE_FILES"
#endif
#if INTERPOSER_FEATURE_RELAY && !INTERPOSER_FEATURE_NETWORK
#error "INTERPOSER_FEATURE_RELAY requires INTERPOSER_FEATURE_NETWORK"
#endif

// Features this platform implements at all. On Linux the registry, DirectInput,
// audio, OS version, relay and rich presence exports are always stubs
// (core/stubs/), whatever the switches say.
#ifdef _WIN32
constexpr DWORD kPlatformFeatures = 0xFFFFFFFFu;
#else
constexpr DWORD kPlatformFeatures =
    INTERPOSER_FEATURE_FLAG_FILES | INTERPOSER_FEATURE_FLAG_FASTDL | INTERPOSER_FEATURE_FLAG_IDENTITY |
    INTERPOSER_FEATURE_FLAG_NETWORK | INTERPOSER_FEATURE_FLAG_MATCHES | INTERPOSER_FEATURE_FLAG_VOICE |
    INTERPOSER_FEATURE_FLAG_PLUGINS;
#endif

// What this image carries: InterposerGetFeatures' answer.
constexpr DWORD kCompiledFeatures = kPlatformFeatures & (0u
    | (INTERPOSER_FEATURE_REGISTRY      ? INTERPOSER_FEATURE_FLAG_REGISTRY      : 0u)
    | (INTERPOSER_FEATURE_FILES         ? INTERPOSER_FEATURE_FLAG_FILES         : 0u)
    | (INTERPOSER_FEATURE_FASTDL        ? INTERPOSER_FEATURE_FLAG_FASTDL        : 0u)
    | (INTERPOSER_FEATURE_DIRECTINPUT   ? INTERPOSER_FEATURE_FLAG_DIRECTINPUT   : 0u)
    | (INTERPOSER_FEATURE_AUDIO         ? INTERPOSER_FEATURE_FLAG_AUDIO         : 0u)
    | (INTERPOSER_FEATURE_OSVERSION     ? INTERPOSER_FEATURE_FLAG_OSVERSION     : 0u)
    | (INTERPOSER_FEATURE_IDENTITY      ? INTERPOSER_FEATURE_FLAG_IDENTITY      : 0u)
    | (INTERPOSER_FEATURE_NETWORK       ? INTERPOSER_FEATURE_FLAG_NETWORK       : 0u)
    | (INTERPOSER_FEATURE_RELAY         ? INTERPOSER_FEATURE_FLAG_RELAY         : 0u)
    | (INTERPOSER_FEATURE_RICH_PRESENCE ? INTERPOSER_FEATURE_FLAG_RICH_PRESENCE : 0u)
    | (INTERPOSER_FEATURE_MATCHES       ? INTERPOSER_FEATURE_FLAG_MATCHES       : 0u)
    | (INTERPOSER_FEATURE_VOICE         ? INTERPOSER_FEATURE_FLAG_VOICE         : 0u)
    | (INTERPOSER_FEATURE_PLUGINS       ? INTERPOSER_FEATURE_FLAG_PLUGINS       : 0u));

// "Registry, Relay" for the features set in mask. Empty for 0.
#include <string>
std::wstring FeatureNames(DWORD mask);

// Called by a copy that lost the election, with the primary it defers to: when
// the primary was built without features this copy carries, says which, in the
// %TEMP% diagnostic log through `diag` and in the primary's session log through
// its InterposerLog. A primary that predates InterposerGetFeatures is a full
// build and is never reported.
void ReportFeaturesMissingFromPrimary(void (*diag)(const wchar_t* message));

// The primary's own feature list, at Debug level, once its log is open. Says
// nothing for a build that carries everything its platform supports.
void LogCompiledFeatures();
