#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------
static void PrintUsage()
{
    wprintf(
        L"LANCommander Interposer Injector\n"
        L"\n"
        L"If <dll-path> is omitted the injector looks for interposer.dll or\n"
        L"LANCommander.Interposer.dll next to itself.\n"
        L"\n"
        L"Inject into a running process:\n"
        L"  Injector.exe [options] <process-name-or-PID> [dll-path]\n"
        L"\n"
        L"Launch a game and inject before its first instruction runs:\n"
        L"  Injector.exe [options] --launch <exe-path> [dll-path]\n"
        L"  Injector.exe [options] --launch <exe-path> [game-args ...] -- [dll-path]\n"
        L"\n"
        L"Options:\n"
        L"  --fastdl-url <url>   Override the FastDL BaseUrl at runtime without editing\n"
        L"                       Config.yml. The URL is passed to the DLL via a\n"
        L"                       named memory-mapped file (Local\\InterposerFastDL_<pid>).\n"
        L"\n"
        L"  --username <name>    Override the username returned by GetUserNameW/A.\n"
        L"                       Passed to the DLL via a named memory-mapped file\n"
        L"                       (Local\\InterposerUsername_<pid>).\n"
        L"\n"
        L"  --computername <n>   Override the computer name returned by\n"
        L"                       GetComputerNameW/A. Passed via a named\n"
        L"                       memory-mapped file (Local\\InterposerComputerName_<pid>).\n"
        L"\n"
        L"Examples:\n"
        L"  Injector.exe game.exe\n"
        L"  Injector.exe game.exe interposer.dll\n"
        L"  Injector.exe 1234\n"
        L"  Injector.exe --fastdl-url http://fastdl.lan/ --launch \"C:\\Games\\game.exe\"\n"
        L"  Injector.exe --username Player1 --launch \"C:\\Games\\game.exe\"\n"
        L"  Injector.exe --computername GAMEPC --launch \"C:\\Games\\game.exe\"\n"
        L"  Injector.exe --launch \"C:\\Games\\game.exe\"\n"
        L"  Injector.exe --launch \"C:\\Games\\game.exe\" interposer.dll\n"
        L"  Injector.exe --launch \"C:\\Games\\game.exe\" -fullscreen -- interposer.dll\n"
        L"  Injector.exe --launch \"C:\\Games\\game.exe\" -fullscreen --\n"
    );
}

// ---------------------------------------------------------------------------
// Default DLL discovery
// ---------------------------------------------------------------------------

// Searches for interposer.dll or LANCommander.Interposer.dll in the same
// directory as the injector executable. Returns the absolute path of the
// first candidate found, or an empty string if neither exists.
static std::wstring FindDefaultDll()
{
    wchar_t selfPath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);

    std::wstring directory(selfPath);
    auto slash = directory.find_last_of(L"\\/");
    
    if (slash != std::wstring::npos)
        directory.resize(slash + 1);

    const wchar_t* candidates[] = {
        L"interposer.dll",
        L"LANCommander.Interposer.dll",
    };

    for (const wchar_t* name : candidates)
    {
        std::wstring path = directory + name;
        
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            return path;
    }
    
    return {};
}

// Resolve an explicit DLL argument to an absolute path, or fall back to the
// default DLL discovery. Writes the result into dllPath[MAX_PATH] and returns
// true on success.
static bool ResolveDll(const wchar_t* dllArg, wchar_t (&dllPath)[MAX_PATH])
{
    if (dllArg)
    {
        if (!GetFullPathNameW(dllArg, MAX_PATH, dllPath, nullptr))
        {
            wprintf(L"[!] Failed to resolve DLL path: %ls\n", dllArg);
            
            return false;
        }
        
        if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES)
        {
            wprintf(L"[!] DLL not found: %ls\n", dllPath);
            
            return false;
        }
    }
    else
    {
        std::wstring found = FindDefaultDll();
        
        if (found.empty())
        {
            wprintf(L"[!] No DLL path given and neither interposer.dll nor "
                    L"LANCommander.Interposer.dll was found next to this executable.\n");
            
            return false;
        }
        
        wcscpy_s(dllPath, found.c_str());
        wprintf(L"[*] Using default DLL: %ls\n", dllPath);
    }
    
    return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Returns true if the process identified by hProcess is a 64-bit native process.
static bool IsProcess64Bit(HANDLE hProcess)
{
    BOOL isWow64 = FALSE;
    IsWow64Process(hProcess, &isWow64);
    
    if (isWow64)
        return false; // WOW64 == 32-bit on 64-bit Windows

    // Check whether the host OS is 64-bit
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    
    bool os64 = (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
                 si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64);
    
    // Not WOW64 on 64-bit OS → native 64-bit process
    return os64;
}

static void WarnBitnessMismatch(HANDLE hProcess)
{
    bool targetIs64  = IsProcess64Bit(hProcess);
    bool selfIs64    = IsProcess64Bit(GetCurrentProcess());
    
    if (targetIs64 != selfIs64)
    {
        wprintf(L"[!] Warning: %ls injector cannot inject into a %ls process.\n"
                L"    Use the matching %ls build of this injector.\n",
            selfIs64 ? L"64-bit" : L"32-bit",
            targetIs64 ? L"64-bit" : L"32-bit",
            targetIs64 ? L"64-bit" : L"32-bit");
    }
}

// Append a single argument to a CreateProcess command line, quoting if needed.
static void AppendArgument(std::wstring& cmdLine, const std::wstring& argument)
{
    if (!cmdLine.empty())
        cmdLine += L' ';

    // Quote if the argument contains spaces or tabs, or is empty
    bool needQuote = argument.empty() || argument.find_first_of(L" \t") != std::wstring::npos;
    
    if (needQuote)
    {
        cmdLine += L'"';
        
        // Escape any embedded double-quotes
        for (wchar_t c : argument)
        {
            if (c == L'"') cmdLine += L'\\';
            cmdLine += c;
        }
        
        cmdLine += L'"';
    }
    else
    {
        cmdLine += argument;
    }
}

// ---------------------------------------------------------------------------
// Process lookup (inject-into-running mode)
// ---------------------------------------------------------------------------
static DWORD FindProcessByName(const wchar_t* processName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W processEntry{};
    processEntry.dwSize = sizeof(processEntry);

    DWORD pid = 0;
    
    if (Process32FirstW(snap, &processEntry))
    {
        do {
            if (_wcsicmp(processEntry.szExeFile, processName) == 0)
            {
                pid = processEntry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &processEntry));
    }

    CloseHandle(snap);
    
    return pid;
}

// ---------------------------------------------------------------------------
// FastDL named MMF helpers
// Creates a named MMF "Local\InterposerFastDL_<pid>" containing the UTF-8
// encoded URL so that InitFastDL() inside the DLL can read it.
// Returns a valid HANDLE on success, or NULL on failure.
// The caller must CloseHandle() the returned handle after injection completes.
// ---------------------------------------------------------------------------
static HANDLE CreateFastDLMapping(DWORD pid, const std::wstring& url)
{
    wchar_t mmfName[64]{};
    wsprintfW(mmfName, L"Local\\InterposerFastDL_%lu", pid);

    HANDLE hMMF = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, 2048, mmfName);

    if (!hMMF)
    {
        wprintf(L"[!] Failed to create FastDL MMF (error %lu).\n", GetLastError());
        return nullptr;
    }

    void* view = MapViewOfFile(hMMF, FILE_MAP_WRITE, 0, 0, 2048);
    if (!view)
    {
        wprintf(L"[!] Failed to map FastDL MMF view (error %lu).\n", GetLastError());
        CloseHandle(hMMF);
        return nullptr;
    }

    // Write URL as UTF-8 with null terminator
    int n = WideCharToMultiByte(CP_UTF8, 0, url.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n > 0 && n <= 2048)
        WideCharToMultiByte(CP_UTF8, 0, url.c_str(), -1, static_cast<char*>(view), n, nullptr, nullptr);

    UnmapViewOfFile(view);
    return hMMF;
}

// ---------------------------------------------------------------------------
// Username named MMF helpers
// Creates a named MMF "Local\InterposerUsername_<pid>" containing the UTF-8
// encoded username so that InstallIdentityHooks() inside the DLL can read it.
// Returns a valid HANDLE on success, or NULL on failure.
// The caller must CloseHandle() the returned handle after injection completes.
// ---------------------------------------------------------------------------
static HANDLE CreateUsernameMapping(DWORD pid, const std::wstring& username)
{
    wchar_t mmfName[64]{};
    wsprintfW(mmfName, L"Local\\InterposerUsername_%lu", pid);

    HANDLE hMMF = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, 512, mmfName);

    if (!hMMF)
    {
        wprintf(L"[!] Failed to create username MMF (error %lu).\n", GetLastError());
        return nullptr;
    }

    void* view = MapViewOfFile(hMMF, FILE_MAP_WRITE, 0, 0, 512);
    if (!view)
    {
        wprintf(L"[!] Failed to map username MMF view (error %lu).\n", GetLastError());
        CloseHandle(hMMF);
        return nullptr;
    }

    // Write username as UTF-8 with null terminator
    int n = WideCharToMultiByte(CP_UTF8, 0, username.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n > 0 && n <= 512)
        WideCharToMultiByte(CP_UTF8, 0, username.c_str(), -1, static_cast<char*>(view), n, nullptr, nullptr);

    UnmapViewOfFile(view);
    return hMMF;
}

// ---------------------------------------------------------------------------
// Computer name named MMF helpers
// Creates a named MMF "Local\InterposerComputerName_<pid>" containing the
// UTF-8 encoded name so that InstallIdentityHooks() inside the DLL can read it.
// Returns a valid HANDLE on success, or NULL on failure.
// The caller must CloseHandle() the returned handle after injection completes.
// ---------------------------------------------------------------------------
static HANDLE CreateComputerNameMapping(DWORD pid, const std::wstring& name)
{
    wchar_t mmfName[64]{};
    wsprintfW(mmfName, L"Local\\InterposerComputerName_%lu", pid);

    HANDLE hMMF = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, 512, mmfName);

    if (!hMMF)
    {
        wprintf(L"[!] Failed to create computer name MMF (error %lu).\n", GetLastError());
        return nullptr;
    }

    void* view = MapViewOfFile(hMMF, FILE_MAP_WRITE, 0, 0, 512);
    if (!view)
    {
        wprintf(L"[!] Failed to map computer name MMF view (error %lu).\n", GetLastError());
        CloseHandle(hMMF);
        return nullptr;
    }

    int n = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n > 0 && n <= 512)
        WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, static_cast<char*>(view), n, nullptr, nullptr);

    UnmapViewOfFile(view);
    return hMMF;
}

// ---------------------------------------------------------------------------
// Core injection
// Injects dllPath into hProcess via a remote LoadLibraryW thread.
// Returns true on success.
// ---------------------------------------------------------------------------
static bool InjectDll(HANDLE hProcess, const wchar_t* dllPath)
{
    // Allocate memory in the target process for the DLL path string
    SIZE_T pathBytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID remoteMem = VirtualAllocEx(hProcess, nullptr, pathBytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    
    if (!remoteMem)
    {
        wprintf(L"[!] VirtualAllocEx failed (error %lu).\n", GetLastError());
        return false;
    }

    SIZE_T written = 0;
    
    if (!WriteProcessMemory(hProcess, remoteMem, dllPath, pathBytes, &written)
        || written != pathBytes)
    {
        wprintf(L"[!] WriteProcessMemory failed (error %lu).\n", GetLastError());
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        
        return false;
    }
    wprintf(L"[*] Wrote DLL path to target process memory.\n");

    // Resolve LoadLibraryW — valid across processes for system DLLs on same-bitness targets
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE loadLib =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(hKernel32, "LoadLibraryW"));

    if (!loadLib)
    {
        wprintf(L"[!] Could not resolve LoadLibraryW.\n");
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        return false;
    }

    wprintf(L"[*] Creating remote thread...\n");
    
    HANDLE threadHandle = CreateRemoteThread(
        hProcess, nullptr, 0,
        loadLib, remoteMem,
        0, nullptr);

    if (!threadHandle)
    {
        wprintf(L"[!] CreateRemoteThread failed (error %lu). Try running as Administrator.\n",
            GetLastError());
        
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        
        return false;
    }

    WaitForSingleObject(threadHandle, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeThread(threadHandle, &exitCode);
    CloseHandle(threadHandle);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);

    if (exitCode == 0)
    {
        wprintf(L"[!] Injection failed: LoadLibraryW returned NULL.\n");
        wprintf(L"    Verify the DLL is a valid %ls binary.\n",
#if defined(_WIN64)
            L"64-bit"
#else
            L"32-bit"
#endif
        );
        
        return false;
    }

    wprintf(L"[+] Injection successful! Module base in target: 0x%08lX\n", exitCode);
    
    return true;
}

// ---------------------------------------------------------------------------
// Inject mode: inject into an already-running process
// ---------------------------------------------------------------------------
static int ModeInject(const wchar_t* target, const wchar_t* dllPath,
                      const std::wstring& fastdlUrl,
                      const std::wstring& username,
                      const std::wstring& computername)
{
    DWORD pid = 0;
    wchar_t* end = nullptr;
    DWORD parsed = static_cast<DWORD>(wcstoul(target, &end, 10));

    if (end != target && *end == L'\0')
    {
        pid = parsed;
        wprintf(L"[*] Using PID: %lu\n", pid);
    }
    else
    {
        wprintf(L"[*] Searching for process: %ls\n", target);

        pid = FindProcessByName(target);

        if (!pid)
        {
            wprintf(L"[!] Process not found: %ls\n", target);
            return 1;
        }

        wprintf(L"[*] Found PID: %lu\n", pid);
    }

    constexpr DWORD ACCESS =
        PROCESS_CREATE_THREAD |
        PROCESS_VM_OPERATION  |
        PROCESS_VM_WRITE      |
        PROCESS_VM_READ       |
        PROCESS_QUERY_INFORMATION;

    HANDLE processHandle = OpenProcess(ACCESS, FALSE, pid);

    if (!processHandle)
    {
        wprintf(L"[!] OpenProcess failed (error %lu). Try running as Administrator.\n",
            GetLastError());
        return 1;
    }

    wprintf(L"[*] Opened process handle.\n");

    WarnBitnessMismatch(processHandle);

    // Create named MMFs before injection so the DLL can read them
    HANDLE hFastDLMMF      = nullptr;
    HANDLE hUsernameMMF    = nullptr;
    HANDLE hComputerNameMMF = nullptr;
    
    if (!fastdlUrl.empty())
    {
        hFastDLMMF = CreateFastDLMapping(pid, fastdlUrl);
        
        if (hFastDLMMF)
            wprintf(L"[*] FastDL URL MMF created for PID %lu.\n", pid);
    }
    
    if (!username.empty())
    {
        hUsernameMMF = CreateUsernameMapping(pid, username);
        
        if (hUsernameMMF)
            wprintf(L"[*] Username MMF created for PID %lu.\n", pid);
    }
    
    if (!computername.empty())
    {
        hComputerNameMMF = CreateComputerNameMapping(pid, computername);
        
        if (hComputerNameMMF)
            wprintf(L"[*] Computer name MMF created for PID %lu.\n", pid);
    }

    bool ok = InjectDll(processHandle, dllPath);

    if (hFastDLMMF)
        CloseHandle(hFastDLMMF);
    if (hUsernameMMF)
        CloseHandle(hUsernameMMF);
    if (hComputerNameMMF)
        CloseHandle(hComputerNameMMF);
    
    CloseHandle(processHandle);

    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Launch mode: create the process suspended, inject, then resume
// ---------------------------------------------------------------------------
static int ModeLaunch(const wchar_t* exePath,
                      const std::vector<std::wstring>& gameArgs,
                      const wchar_t* dllPath,
                      const std::wstring& fastdlUrl,
                      const std::wstring& username,
                      const std::wstring& computername)
{
    // Verify the executable exists
    if (GetFileAttributesW(exePath) == INVALID_FILE_ATTRIBUTES)
    {
        wprintf(L"[!] Executable not found: %ls\n", exePath);
        return 1;
    }

    // Resolve exe to an absolute path so the child's working directory isn't ambiguous
    wchar_t exeFullPath[MAX_PATH]{};
    
    if (!GetFullPathNameW(exePath, MAX_PATH, exeFullPath, nullptr))
    {
        wprintf(L"[!] Failed to resolve executable path: %ls\n", exePath);
        return 1;
    }

    // Build command line: "<exe>" [args...]
    std::wstring cmdLine;
    
    AppendArgument(cmdLine, exeFullPath);
    
    for (const auto& arg : gameArgs)
        AppendArgument(cmdLine, arg);

    // Set the working directory to the directory containing the exe
    std::wstring workingDirectory(exeFullPath);
    
    auto slash = workingDirectory.find_last_of(L"\\/");
    
    if (slash != std::wstring::npos)
        workingDirectory.resize(slash);
    else 
        workingDirectory.clear();

    wprintf(L"[*] Launching: %ls\n", cmdLine.c_str());

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInformation{};

    if (!CreateProcessW(
            exeFullPath,
            cmdLine.data(),          // must be mutable
            nullptr, nullptr,
            FALSE,
            CREATE_SUSPENDED,        // suspend before first instruction
            nullptr,
            workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
            &startupInfo, &processInformation))
    {
        wprintf(L"[!] CreateProcess failed (error %lu).\n", GetLastError());
        
        return 1;
    }

    wprintf(L"[*] Process created (PID %lu), suspended. Injecting...\n", processInformation.dwProcessId);

    WarnBitnessMismatch(processInformation.hProcess);

    // Create named MMFs before injection so the DLL can read them
    HANDLE hFastDLMMF       = nullptr;
    HANDLE hUsernameMMF     = nullptr;
    HANDLE hComputerNameMMF = nullptr;
    
    if (!fastdlUrl.empty())
    {
        hFastDLMMF = CreateFastDLMapping(processInformation.dwProcessId, fastdlUrl);
        if (hFastDLMMF)
            wprintf(L"[*] FastDL URL MMF created for PID %lu.\n", processInformation.dwProcessId);
    }
    
    if (!username.empty())
    {
        hUsernameMMF = CreateUsernameMapping(processInformation.dwProcessId, username);
        if (hUsernameMMF)
            wprintf(L"[*] Username MMF created for PID %lu.\n", processInformation.dwProcessId);
    }
    
    if (!computername.empty())
    {
        hComputerNameMMF = CreateComputerNameMapping(processInformation.dwProcessId, computername);
        if (hComputerNameMMF)
            wprintf(L"[*] Computer name MMF created for PID %lu.\n", processInformation.dwProcessId);
    }

    bool ok = InjectDll(processInformation.hProcess, dllPath);

    if (hFastDLMMF)
        CloseHandle(hFastDLMMF);
    
    if (hUsernameMMF)
        CloseHandle(hUsernameMMF);
    
    if (hComputerNameMMF)
        CloseHandle(hComputerNameMMF);

    if (!ok)
    {
        wprintf(L"[!] Injection failed; terminating the suspended process.\n");
        TerminateProcess(processInformation.hProcess, 1);
        CloseHandle(processInformation.hThread);
        CloseHandle(processInformation.hProcess);

        return 1;
    }

    // Resume the game's main thread — it will now run normally with our DLL already loaded
    wprintf(L"[*] Resuming process...\n");
    ResumeThread(processInformation.hThread);

    wprintf(L"[+] %ls is running (PID %lu).\n", exeFullPath, processInformation.dwProcessId);

    CloseHandle(processInformation.hThread);
    CloseHandle(processInformation.hProcess);
    
    return 0;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int wmain(int argc, wchar_t* argv[])
{
    // ---- Pre-scan for global options
    std::wstring fastdlUrl;
    std::wstring username;
    std::wstring computername;
    std::vector<wchar_t*> args;

    for (int i = 1; i < argc; ++i)
    {
        if (wcscmp(argv[i], L"--fastdl-url") == 0)
        {
            if (i + 1 >= argc)
            {
                wprintf(L"[!] --fastdl-url requires a URL argument.\n\n");
                PrintUsage();
                return 1;
            }
            fastdlUrl = argv[++i];
        }
        else if (wcscmp(argv[i], L"--username") == 0)
        {
            if (i + 1 >= argc)
            {
                wprintf(L"[!] --username requires a name argument.\n\n");
                PrintUsage();
                return 1;
            }
            username = argv[++i];
        }
        else if (wcscmp(argv[i], L"--computername") == 0)
        {
            if (i + 1 >= argc)
            {
                wprintf(L"[!] --computername requires a name argument.\n\n");
                PrintUsage();
                return 1;
            }
            computername = argv[++i];
        }
        else
        {
            args.push_back(argv[i]);
        }
    }

    int filteredArgc = static_cast<int>(args.size());

    // ---- Launch mode: --launch <exe> [args...] -- <dll>
    //                or --launch <exe> <dll>               (no game args)
    if (filteredArgc >= 1 && wcscmp(args[0], L"--launch") == 0)
    {
        if (filteredArgc < 2)
        {
            wprintf(L"[!] --launch requires at least an executable path.\n\n");
            PrintUsage();
            return 1;
        }

        const wchar_t* exeArguments = args[1];
        const wchar_t* dllArguments = nullptr;
        std::vector<std::wstring> gameArguments;

        // Scan for the "--" separator
        int separatorIndex = -1;

        for (int i = 2; i < filteredArgc; ++i)
        {
            if (wcscmp(args[i], L"--") == 0)
            {
                separatorIndex = i;
                break;
            }
        }

        if (separatorIndex >= 0)
        {
            // Everything before "--" is game args; the first token after "--" (if
            // any) is the DLL path. Omitting it triggers default DLL discovery.
            for (int i = 2; i < separatorIndex; ++i)
                gameArguments.push_back(args[i]);

            if (separatorIndex + 1 < filteredArgc)
                dllArguments = args[separatorIndex + 1];
        }
        else if (filteredArgc >= 3)
        {
            // No "--" separator. If the last argument ends in ".dll" (case-insensitive)
            // treat it as the DLL path; otherwise treat all extra arguments as game
            // args and fall back to default DLL discovery.
            const wchar_t* last = args[filteredArgc - 1];
            size_t length = wcslen(last);

            bool lastIsDll = (length >= 4 && _wcsicmp(last + length - 4, L".dll") == 0);

            if (lastIsDll)
            {
                dllArguments = last;

                for (int i = 2; i < filteredArgc - 1; ++i)
                    gameArguments.push_back(args[i]);
            }
            else
            {
                // No DLL arg; all extra tokens are game args
                for (int i = 2; i < filteredArgc; ++i)
                    gameArguments.push_back(args[i]);
            }
        }
        // filteredArgc == 2: just "--launch <exe>", no extra args → dllArg stays null

        wchar_t dllPath[MAX_PATH]{};

        if (!ResolveDll(dllArguments, dllPath))
            return 1;

        return ModeLaunch(exeArguments, gameArguments, dllPath, fastdlUrl, username, computername);
    }

    // ---- Inject mode: <process-name-or-PID> [dll]
    if (filteredArgc == 1 || filteredArgc == 2)
    {
        const wchar_t* dllArguments = (filteredArgc == 2) ? args[1] : nullptr;

        wchar_t dllPath[MAX_PATH]{};
        if (!ResolveDll(dllArguments, dllPath))
            return 1;

        return ModeInject(args[0], dllPath, fastdlUrl, username, computername);
    }

    // ---- Bad args
    PrintUsage();

    return 1;
}
