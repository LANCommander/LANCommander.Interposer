#include "fastdl.h"

// The network hooks are what collect game-server addresses for FastDL to probe.
// Until Linux has them nothing is collected, so there is nothing to probe and
// FastDL works from a configured BaseUrl (or the launcher's override) alone.
void ProbeAllDiscoveredAddresses()
{
}
