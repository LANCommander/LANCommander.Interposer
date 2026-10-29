#pragma once

// The NetworkAdapters allow-list, evaluated against the config globals (g_na*).
// An adapter is kept if it matches ANY populated list. Shared by the Windows
// adapter-enumeration hooks and the Linux ones, so one adapter is judged the
// same way wherever it is enumerated.

#include <cstddef>
#include <cstdint>
#include <string>

namespace netfilter
{
    // The filter is on: NetworkAdapters.Enabled, with at least one list populated.
    // When false every adapter is kept and these predicates need not be consulted.
    bool Active();

    // ipHostOrder: an IPv4 address in HOST byte order.
    bool SubnetMatch(uint32_t ipHostOrder);

    // A six-byte physical address.
    bool MacMatch(const uint8_t* address, size_t length);

    // vs the NetworkAdapters.Names regexes (FriendlyName, Description).
    bool NameMatch(const std::wstring& name);
}
