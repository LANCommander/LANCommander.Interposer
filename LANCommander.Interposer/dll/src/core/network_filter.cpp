#include "network_filter.h"
#include "config.h"

#include <cstring>
#include <regex>

namespace netfilter
{

bool Active()
{
    return g_naEnabled && g_naAnyFilters;
}

bool SubnetMatch(uint32_t ipHostOrder)
{
    for (const auto& s : g_naSubnets)
        if ((ipHostOrder & s.mask) == s.network)
            return true;

    return false;
}

bool MacMatch(const uint8_t* address, size_t length)
{
    if (!address || length != 6)
        return false;

    for (const auto& m : g_naMacs)
        if (std::memcmp(m.data(), address, 6) == 0)
            return true;

    return false;
}

bool NameMatch(const std::wstring& name)
{
    if (name.empty())
        return false;

    try
    {
        for (const auto& re : g_naNames)
            if (std::regex_search(name, re))
                return true;
    }
    catch (const std::regex_error&) { /* pathological input — treat as no match */ }

    return false;
}

} // namespace netfilter
