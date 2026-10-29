#pragma once

// Apply any launcher identity overrides and log the computer-name override that
// the gethostname / uname interposers serve. Called from InstallHooks.
void InstallIdentityHooks();
