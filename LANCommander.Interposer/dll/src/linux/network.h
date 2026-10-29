#pragma once

// Log which DNS-redirect hooks are active. The hooks themselves are exported
// libc interposers (getaddrinfo, gethostbyname); this only records them, now
// that the session log exists. Called from InstallHooks.
void InstallNetworkHooks();
