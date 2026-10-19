// Lanceur : injection classique (CreateRemoteThread + LoadLibraryW) de notre DLL
// dans le processus explorer.exe qui heberge les fenetres de l'Explorateur.
// N'utilise PAS le diagnostic XAML (donc aucun fail-fast WUX).
#include <windows.h>
#include <psapi.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <cstdio>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shlwapi.lib")

static std::vector<DWORD> ExplorerPids()
{
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(snap, &pe))
        do { if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) pids.push_back(pe.th32ProcessID); }
        while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return pids;
}

// Vrai si le processus a charge FileExplorerExtensions.dll (= fenetres Explorateur).
static bool HasFileExplorerUi(DWORD pid)
{
    bool found = false;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return false;
    HMODULE mods[1024]; DWORD need = 0;
    if (EnumProcessModulesEx(h, mods, sizeof(mods), &need, LIST_MODULES_ALL))
    {
        const DWORD n = need / sizeof(HMODULE);
        for (DWORD i = 0; i < n && !found; ++i)
        {
            wchar_t p[MAX_PATH];
            if (GetModuleFileNameExW(h, mods[i], p, MAX_PATH) &&
                StrStrIW(p, L"FileExplorerExtensions.dll"))
                found = true;
        }
    }
    CloseHandle(h);
    return found;
}

static bool InjectInto(DWORD pid, const std::wstring& dll)
{
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                           FALSE, pid);
    if (!h) { wprintf(L"[x] OpenProcess %lu a echoue (%lu)\n", pid, GetLastError()); return false; }

    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(h, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = false;
    if (remote && WriteProcessMemory(h, remote, dll.c_str(), bytes, nullptr))
    {
        auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE th = CreateRemoteThread(h, nullptr, 0, loadLib, remote, 0, nullptr);
        if (th)
        {
            WaitForSingleObject(th, 10000);
            DWORD code = 0; GetExitCodeThread(th, &code); // handle 32 bits tronque, informatif
            CloseHandle(th);
            ok = true;
        }
        else wprintf(L"[x] CreateRemoteThread a echoue (%lu)\n", GetLastError());
    }
    if (remote) VirtualFreeEx(h, remote, 0, MEM_RELEASE);
    CloseHandle(h);
    return ok;
}

int wmain(int argc, wchar_t** argv)
{
    std::wstring dll;
    if (argc >= 2) dll = argv[1];
    else
    {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        PathRemoveFileSpecW(self);
        PathCombineW(self, self, L"ExplorerSearchExt.dll");
        dll = self;
    }
    if (!PathFileExistsW(dll.c_str())) { wprintf(L"[x] DLL introuvable : %s\n", dll.c_str()); return 2; }
    wprintf(L"[*] DLL : %s\n", dll.c_str());

    int injected = 0;
    for (DWORD pid : ExplorerPids())
    {
        if (!HasFileExplorerUi(pid)) continue;
        wprintf(L"[*] injection dans explorer.exe PID %lu...\n", pid);
        if (InjectInto(pid, dll)) { wprintf(L"[+] OK\n"); ++injected; }
    }
    if (!injected)
    {
        wprintf(L"[x] Aucune fenetre Explorateur trouvee. Ouvre-en une puis relance.\n");
        return 3;
    }
    wprintf(L"[+] Injecte. Clique dans le champ de recherche d'une fenetre Explorateur.\n");
    return 0;
}
