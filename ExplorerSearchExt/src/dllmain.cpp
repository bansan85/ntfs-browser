// INPROC (solution pipeline-hook) : DLL injectee dans explorer.exe.
// - expose l'extension d'espace de noms (CLSID) via les exports COM ;
// - enregistre le CLSID (permanent, HKCU) et pose le hook du pipeline de
//   recherche sur un thread dedie (hors loader lock).
#include <windows.h>
#include <objbase.h>
#include "Options.h"
#include "PipelineHook.h"
#include "SearchNse.h"

HINSTANCE g_hinst = nullptr;

SearchOptions& Options()
{
    static SearchOptions o;
    return o;
}

static DWORD WINAPI InstallThread(LPVOID)
{
    SearchNse_RegisterServer();   // enregistrement permanent (1 fois, idempotent)
    InstallPipelineHook();        // hook du pipeline natif
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hinst = hinst;
        DisableThreadLibraryCalls(hinst);
        CloseHandle(CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr));
    }
    return TRUE;
}

// --- Exports COM (voir ExplorerSearchExt.def) ------------------------------
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (rclsid == CLSID_ExplorerSearchNse)
        return SearchNse_GetClassObject(riid, ppv);
    *ppv = nullptr;
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow()
{
    return S_FALSE; // injectee : on reste chargee
}

STDAPI DllRegisterServer()   { return SearchNse_RegisterServer(); }
STDAPI DllUnregisterServer() { return SearchNse_UnregisterServer(); }
