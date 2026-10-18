// ==WindhawkMod==
// @id              nvidia-app-tray-icon
// @name            NVIDIA App Tray Icon
// @description     Keeps the NVIDIA App tray icon shown (or hidden) with a toggle, re-applying the fix whenever a driver update resets it
// @version         1.1.0
// @author          halftones
// @github          https://github.com/precompile
// @license         MIT
// @include         windhawk.exe
// @compilerOptions -lshell32 -ladvapi32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# NVIDIA App Tray Icon

NVIDIA driver updates often reset the registry values that make the NVIDIA App
("NVIDIA Settings") icon appear in the notification area, so the icon silently
disappears after every update while the *GPU activity* icon stays.

This mod keeps these values in the state you pick:

- `HKLM\SYSTEM\CurrentControlSet\Services\nvlddmkm\Parameters\NVTray`
  - `StartOnLogin`
  - `ShowInSedona`
- `HKCU\Software\NVIDIA Corporation\NvTray`
  - `StartOnLogin`

It watches these keys and re-checks them every minute, so when a driver update
flips the values back to `0` the mod sets them again. When it has to change
the machine-wide values, it restarts the **NVIDIA Display Container LS**
service so the icon comes back immediately, without a reboot or sign-out.

## How it runs

- The machine-wide (`HKLM`) values and the service restart are handled inside
  the Windhawk service, which runs as SYSTEM. That's the only part of Windhawk
  allowed to write that key.
- The per-user (`HKCU`) value is handled by a small background Windhawk tool
  process running as you.

## Usage

- **Show NVIDIA App tray icon** on → icon shown. Off → icon hidden.
- If the icon appears only in the overflow (`^`) flyout, drag it onto the
  taskbar, or enable it in *Settings → Personalization → Taskbar → Other
  system tray icons*.

## Notes

- Disabling the mod leaves the registry values as they are.
- Only NVIDIA systems are touched: if the NVIDIA driver's registry keys don't
  exist, the mod does nothing.
*/
// ==/WindhawkModReadme==

// clang-format off
// ==WindhawkModSettings==
/*
- showTrayIcon: true
  $name: Show NVIDIA App tray icon
  $description: On shows the NVIDIA App tray icon, off hides it. The choice is re-applied after driver updates.
- restartContainer: true
  $name: Restart NVIDIA Display Container when needed
  $description: After the mod changes a machine-wide value, restart the "NVIDIA Display Container LS" service so the change shows up right away instead of after a reboot.
*/
// ==/WindhawkModSettings==
// clang-format on

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cwchar>

namespace {

// A registry key the mod keeps in the wanted state. The key is only created
// if its parent already exists, so nothing is added on machines without
// NVIDIA software.
struct Scope {
    PCWSTR label;
    HKEY root;
    PCWSTR parentPath;
    PCWSTR subKey;
    PCWSTR fullPath;
    REGSAM extraSam;
    const PCWSTR* values;
    size_t valueCount;
    // Whether a change here needs the display container restarted. Only the
    // SYSTEM-side scope can do that.
    bool restartsContainer;
};

constexpr PCWSTR kMachineValues[] = {L"StartOnLogin", L"ShowInSedona"};
constexpr PCWSTR kUserValues[] = {L"StartOnLogin"};

const Scope kMachineScope = {
    L"HKLM",
    HKEY_LOCAL_MACHINE,
    L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\Parameters",
    L"NVTray",
    L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\Parameters\\NVTray",
    KEY_WOW64_64KEY,
    kMachineValues,
    ARRAYSIZE(kMachineValues),
    true,
};

const Scope kUserScope = {
    L"HKCU",
    HKEY_CURRENT_USER,
    L"Software\\NVIDIA Corporation",
    L"NvTray",
    L"Software\\NVIDIA Corporation\\NvTray",
    0,
    kUserValues,
    ARRAYSIZE(kUserValues),
    false,
};

constexpr WCHAR kContainerService[] = L"NVDisplay.ContainerLocalSystem";

// Periodic re-check also catches the key being deleted and recreated by a
// driver install, which invalidates the change notification handle.
constexpr DWORD kRecheckIntervalMs = 60 * 1000;
// Let a driver installer finish its burst of writes before reacting.
constexpr DWORD kChangeDebounceMs = 1500;
// If something keeps reverting the values, don't restart the service in a
// loop; settings changes made by the user bypass this.
constexpr ULONGLONG kExternalRestartCooldownMs = 5 * 60 * 1000;
constexpr DWORD kServiceStopTimeoutMs = 15 * 1000;

std::atomic<bool> g_showTrayIcon{true};
std::atomic<bool> g_restartContainer{true};

const Scope* g_scope;
HANDLE g_stopEvent;
HANDLE g_settingsEvent;
HANDLE g_workerThread;

void LoadSettings() {
    g_showTrayIcon = Wh_GetIntSetting(L"showTrayIcon") != 0;
    g_restartContainer = Wh_GetIntSetting(L"restartContainer") != 0;
}

enum class EnsureResult { Unchanged, Changed, Failed };

// Sets a REG_DWORD only when it's missing or different.
EnsureResult EnsureDword(HKEY key, PCWSTR name, DWORD wanted) {
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    LSTATUS status = RegQueryValueExW(key, name, nullptr, &type,
                                      (BYTE*)&value, &size);
    if (status == ERROR_SUCCESS && type == REG_DWORD &&
        size == sizeof(value) && value == wanted) {
        return EnsureResult::Unchanged;
    }

    status = RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&wanted,
                            sizeof(wanted));
    if (status != ERROR_SUCCESS) {
        Wh_Log(L"Setting %s failed: %ld", name, status);
        return EnsureResult::Failed;
    }

    Wh_Log(L"Set %s = %u", name, wanted);
    return EnsureResult::Changed;
}

HKEY OpenScopeForWrite(const Scope& scope) {
    HKEY parent;
    LSTATUS status =
        RegOpenKeyExW(scope.root, scope.parentPath, 0,
                      KEY_CREATE_SUB_KEY | scope.extraSam, &parent);
    if (status != ERROR_SUCCESS) {
        if (status != ERROR_FILE_NOT_FOUND) {
            Wh_Log(L"Opening %s\\%s failed: %ld", scope.label,
                   scope.parentPath, status);
        }
        return nullptr;
    }

    HKEY child = nullptr;
    status = RegCreateKeyExW(parent, scope.subKey, 0, nullptr,
                             REG_OPTION_NON_VOLATILE,
                             KEY_QUERY_VALUE | KEY_SET_VALUE | scope.extraSam,
                             nullptr, &child, nullptr);
    RegCloseKey(parent);
    if (status != ERROR_SUCCESS) {
        Wh_Log(L"Opening %s\\%s failed: %ld", scope.label, scope.fullPath,
               status);
        return nullptr;
    }

    return child;
}

// Returns true if any value was changed.
bool ApplyTrayValues(const Scope& scope, bool show) {
    HKEY key = OpenScopeForWrite(scope);
    if (!key) {
        return false;
    }

    DWORD wanted = show ? 1 : 0;
    bool changed = false;
    for (size_t i = 0; i < scope.valueCount; i++) {
        changed |= EnsureDword(key, scope.values[i], wanted) ==
                   EnsureResult::Changed;
    }

    RegCloseKey(key);
    return changed;
}

bool WaitForServiceState(SC_HANDLE service, DWORD state, DWORD timeoutMs) {
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    SERVICE_STATUS status;
    while (QueryServiceStatus(service, &status)) {
        if (status.dwCurrentState == state) {
            return true;
        }
        if (GetTickCount64() >= deadline) {
            break;
        }
        Sleep(250);
    }
    return false;
}

void RestartDisplayContainer() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        Wh_Log(L"OpenSCManager failed: %u", GetLastError());
        return;
    }

    SC_HANDLE service =
        OpenServiceW(scm, kContainerService,
                     SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
    if (!service) {
        Wh_Log(L"OpenService(%s) failed: %u", kContainerService,
               GetLastError());
        CloseServiceHandle(scm);
        return;
    }

    Wh_Log(L"Restarting %s", kContainerService);

    SERVICE_STATUS status;
    if (ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        // Bounded and deliberately not interruptible: bailing out mid-stop
        // could leave the service stopped.
        if (!WaitForServiceState(service, SERVICE_STOPPED,
                                 kServiceStopTimeoutMs)) {
            Wh_Log(L"Service didn't stop in time");
        }
    } else if (GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        Wh_Log(L"Stopping service failed: %u", GetLastError());
    }

    if (!StartServiceW(service, 0, nullptr) &&
        GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        Wh_Log(L"Starting service failed: %u", GetLastError());
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
}

// Arms a one-shot change notification on the scope's key. Returns false (and
// closes the key) if the key is gone, so the caller reopens it later.
bool ArmWatch(const Scope& scope, HKEY* watchKey, HANDLE event) {
    if (!*watchKey) {
        // Closing a key with a pending notification signals its event, so
        // clear any stale signal before arming a fresh handle.
        ResetEvent(event);
        if (RegOpenKeyExW(scope.root, scope.fullPath, 0,
                          KEY_NOTIFY | scope.extraSam,
                          watchKey) != ERROR_SUCCESS) {
            *watchKey = nullptr;
            return false;
        }
    }

    if (RegNotifyChangeKeyValue(
            *watchKey, FALSE,
            REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, event,
            TRUE) != ERROR_SUCCESS) {
        RegCloseKey(*watchKey);
        *watchKey = nullptr;
        return false;
    }

    return true;
}

DWORD WINAPI WorkerThread(LPVOID) {
    const Scope& scope = *g_scope;

    HANDLE regEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!regEvent) {
        Wh_Log(L"CreateEvent failed");
        return 0;
    }

    HKEY watchKey = nullptr;
    ULONGLONG lastExternalRestart = 0;
    bool userInitiated = true;  // First pass counts as a user action.

    while (true) {
        bool changed = ApplyTrayValues(scope, g_showTrayIcon);
        if (changed && scope.restartsContainer && g_restartContainer) {
            ULONGLONG now = GetTickCount64();
            if (userInitiated) {
                RestartDisplayContainer();
            } else if (!lastExternalRestart ||
                       now - lastExternalRestart >=
                           kExternalRestartCooldownMs) {
                lastExternalRestart = now;
                RestartDisplayContainer();
            } else {
                Wh_Log(L"Values were reverted again; skipping restart "
                       L"(cooldown)");
            }
        }

        ArmWatch(scope, &watchKey, regEvent);

        HANDLE handles[] = {g_stopEvent, g_settingsEvent, regEvent};
        DWORD wait = WaitForMultipleObjects(ARRAYSIZE(handles), handles,
                                            FALSE, kRecheckIntervalMs);
        if (wait == WAIT_OBJECT_0) {
            break;
        }

        userInitiated = wait == WAIT_OBJECT_0 + 1;

        if (wait == WAIT_OBJECT_0 + 2) {
            if (WaitForSingleObject(g_stopEvent, kChangeDebounceMs) ==
                WAIT_OBJECT_0) {
                break;
            }
        } else if (wait == WAIT_TIMEOUT && watchKey) {
            // Reopen periodically in case the key was replaced underneath us.
            RegCloseKey(watchKey);
            watchKey = nullptr;
        }
    }

    if (watchKey) {
        RegCloseKey(watchKey);
    }
    CloseHandle(regEvent);
    return 0;
}

bool StartWorker(const Scope& scope) {
    LoadSettings();
    g_scope = &scope;

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_settingsEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_stopEvent || !g_settingsEvent) {
        Wh_Log(L"CreateEvent failed");
        return false;
    }

    g_workerThread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0,
                                  nullptr);
    if (!g_workerThread) {
        Wh_Log(L"CreateThread failed");
        return false;
    }

    Wh_Log(L"Worker started for %s", scope.label);
    return true;
}

void NotifyWorkerSettingsChanged() {
    LoadSettings();
    if (g_settingsEvent) {
        SetEvent(g_settingsEvent);
    }
}

void StopWorker() {
    if (g_workerThread) {
        SetEvent(g_stopEvent);
        WaitForSingleObject(g_workerThread, INFINITE);
        CloseHandle(g_workerThread);
        g_workerThread = nullptr;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    if (g_settingsEvent) {
        CloseHandle(g_settingsEvent);
        g_settingsEvent = nullptr;
    }
}

}  // namespace

// The tool mod process runs as the signed-in user without elevation, so it
// only handles the per-user value. The machine-wide value is handled in the
// Windhawk service (see Wh_ModInit).
BOOL WhTool_ModInit() {
    return StartWorker(kUserScope);
}

void WhTool_ModSettingsChanged() {
    NotifyWorkerSettingsChanged();
}

void WhTool_ModUninit() {
    StopWorker();
}

// The Windhawk service (windhawk.exe -service) runs as SYSTEM, which is the
// only Windhawk process allowed to write the driver's HKLM key and restart
// the NVIDIA service.
bool g_isServiceProcess;

// clang-format off
// ============================================================================
// Tool Mod Boilerplate (adapted: the -service process runs the HKLM worker)
// ============================================================================
bool g_isToolModProcessLauncher;
HANDLE g_toolModProcessMutex;

void WINAPI EntryPoint_Hook() {
    Wh_Log(L">");
    ExitThread(0);
}

BOOL Wh_ModInit() {
    bool isService = false;
    bool isServiceHelper = false;
    bool isToolModProcess = false;
    bool isCurrentToolModProcess = false;

    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLine(), &argc);
    if (!argv) {
        Wh_Log(L"CommandLineToArgvW failed");
        return FALSE;
    }

    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"-service") == 0) {
            isService = true;
            break;
        }
        if (wcscmp(argv[i], L"-service-start") == 0 ||
            wcscmp(argv[i], L"-service-stop") == 0) {
            isServiceHelper = true;
            break;
        }
    }

    for (int i = 1; i < argc - 1; i++) {
        if (wcscmp(argv[i], L"-tool-mod") == 0) {
            isToolModProcess = true;
            if (wcscmp(argv[i + 1], WH_MOD_ID) == 0) {
                isCurrentToolModProcess = true;
            }
            break;
        }
    }

    LocalFree(argv);

    if (isServiceHelper) {
        return FALSE;
    }

    if (isService) {
        g_isServiceProcess = true;
        return StartWorker(kMachineScope);
    }

    DWORD sessionId;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) &&
        sessionId == 0) {
        return FALSE;
    }

    if (isCurrentToolModProcess) {
        g_toolModProcessMutex =
            CreateMutex(nullptr, TRUE, L"windhawk-tool-mod_" WH_MOD_ID);
        if (!g_toolModProcessMutex) {
            Wh_Log(L"CreateMutex failed");
            ExitProcess(1);
        }

        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            Wh_Log(L"Tool mod already running (%s)", WH_MOD_ID);
            ExitProcess(1);
        }

        if (!WhTool_ModInit()) {
            ExitProcess(1);
        }

        IMAGE_DOS_HEADER* dosHeader =
            (IMAGE_DOS_HEADER*)GetModuleHandle(nullptr);
        IMAGE_NT_HEADERS* ntHeaders =
            (IMAGE_NT_HEADERS*)((BYTE*)dosHeader + dosHeader->e_lfanew);

        DWORD entryPointRVA = ntHeaders->OptionalHeader.AddressOfEntryPoint;
        void* entryPoint = (BYTE*)dosHeader + entryPointRVA;

        Wh_SetFunctionHook(entryPoint, (void*)EntryPoint_Hook, nullptr);
        return TRUE;
    }

    if (isToolModProcess) {
        return FALSE;
    }

    g_isToolModProcessLauncher = true;
    return TRUE;
}

void Wh_ModAfterInit() {
    if (!g_isToolModProcessLauncher) {
        return;
    }

    WCHAR currentProcessPath[MAX_PATH];
    switch (GetModuleFileName(nullptr, currentProcessPath,
                              ARRAYSIZE(currentProcessPath))) {
        case 0:
        case ARRAYSIZE(currentProcessPath):
            Wh_Log(L"GetModuleFileName failed");
            return;
    }

    WCHAR
    commandLine[MAX_PATH + 2 +
                (sizeof(L" -tool-mod \"" WH_MOD_ID "\"") / sizeof(WCHAR)) - 1];
    swprintf_s(commandLine, L"\"%s\" -tool-mod \"%s\"", currentProcessPath,
               WH_MOD_ID);

    HMODULE kernelModule = GetModuleHandle(L"kernelbase.dll");
    if (!kernelModule) {
        kernelModule = GetModuleHandle(L"kernel32.dll");
        if (!kernelModule) {
            Wh_Log(L"No kernelbase.dll/kernel32.dll");
            return;
        }
    }

    using CreateProcessInternalW_t = BOOL(WINAPI*)(
        HANDLE hUserToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
        LPSECURITY_ATTRIBUTES lpProcessAttributes,
        LPSECURITY_ATTRIBUTES lpThreadAttributes, WINBOOL bInheritHandles,
        DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
        LPSTARTUPINFOW lpStartupInfo,
        LPPROCESS_INFORMATION lpProcessInformation,
        PHANDLE hRestrictedUserToken);

    CreateProcessInternalW_t pCreateProcessInternalW =
        (CreateProcessInternalW_t)GetProcAddress(kernelModule,
                                                 "CreateProcessInternalW");
    if (!pCreateProcessInternalW) {
        Wh_Log(L"No CreateProcessInternalW");
        return;
    }

    STARTUPINFO si{
        .cb = sizeof(STARTUPINFO),
        .dwFlags = STARTF_FORCEOFFFEEDBACK,
    };
    PROCESS_INFORMATION pi;
    if (!pCreateProcessInternalW(nullptr, currentProcessPath, commandLine,
                                 nullptr, nullptr, FALSE, NORMAL_PRIORITY_CLASS,
                                 nullptr, nullptr, &si, &pi, nullptr)) {
        Wh_Log(L"CreateProcess failed");
        return;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

void Wh_ModSettingsChanged() {
    if (g_isToolModProcessLauncher) {
        return;
    }

    if (g_isServiceProcess) {
        NotifyWorkerSettingsChanged();
        return;
    }

    WhTool_ModSettingsChanged();
}

void Wh_ModUninit() {
    if (g_isToolModProcessLauncher) {
        return;
    }

    if (g_isServiceProcess) {
        // Never exit the Windhawk service itself, just stop our thread.
        StopWorker();
        return;
    }

    WhTool_ModUninit();
    ExitProcess(0);
}
// clang-format on
