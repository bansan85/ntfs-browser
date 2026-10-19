#include <windows.h>
#include <intrin.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <servprov.h>
#include <dbghelp.h>
#include <psapi.h>
#include <string>
#include "PipelineHook.h"
#include "SearchNse.h"
#include "Options.h"
#include "MinHook.h"

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shlwapi.lib")

static void PLog(const char* m)
{
    char p[MAX_PATH]; if (!GetEnvironmentVariableA("TEMP", p, MAX_PATH)) return;
    strcat_s(p, "\\ExplorerSearchExt.log");
    HANDLE h = CreateFileA(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char b[512]; int n = sprintf_s(b, "[%lu] %s\r\n", GetCurrentThreadId(), m);
    DWORD w; WriteFile(h, b, n, &w, nullptr); CloseHandle(h);
}

// --- Resolution d'adresse via DbgHelp sur le PDB (SymbolCache) --------------
struct FindCtx { const char* needle1; const char* needle2; DWORD64 addr; };
static BOOL CALLBACK EnumCb(PSYMBOL_INFO s, ULONG, PVOID ctx)
{
    auto* r = static_cast<FindCtx*>(ctx);
    if (s->Name && strstr(s->Name, r->needle1) && (!*r->needle2 || strstr(s->Name, r->needle2)))
    { r->addr = s->Address; return FALSE; }
    return TRUE;
}

// Chemin de symboles couvrant la disposition symsrv ...\<pdb>\<GUID>\[stripped].
static std::wstring BuildSymPath(const wchar_t* pdbName)
{
    wchar_t tmp[MAX_PATH]; GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH);
    std::wstring root = std::wstring(tmp) + L"\\SymbolCache";
    std::wstring path = root;
    std::wstring glob = root + L"\\" + pdbName + L"\\*";
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(glob.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
            {
                std::wstring g = root + L"\\" + pdbName + L"\\" + fd.cFileName;
                path += L";" + g + L";" + g + L"\\stripped";
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return path;
}

static DWORD64 ResolveSym(DWORD64 modBase, const char* mask, const char* n1, const char* n2)
{
    FindCtx r{ n1, n2 ? n2 : "", 0 };
    SymEnumSymbols(GetCurrentProcess(), modBase, mask, EnumCb, &r);
    return r.addr;
}

// Resout les RVA voulues dans 'mod' via son PDB (SymbolCache), puis decharge.
// Chaque entree {mask,n1,n2,out} est renseignee avec l'adresse absolue trouvee.
struct SymReq { const char* mask; const char* n1; const char* n2; DWORD64* out; };
static void ResolveModuleSyms(HMODULE mod, const wchar_t* pdbName, SymReq* reqs, int n)
{
    wchar_t modPath[MAX_PATH]; GetModuleFileNameW(mod, modPath, MAX_PATH);
    std::wstring sympath = BuildSymPath(pdbName);
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_EXACT_SYMBOLS);
    SymInitializeW(proc, sympath.c_str(), FALSE);
    MODULEINFO mi{}; GetModuleInformation(proc, mod, &mi, sizeof(mi));
    DWORD64 base = SymLoadModuleExW(proc, nullptr, modPath, nullptr, (DWORD64)mod, mi.SizeOfImage, nullptr, 0);
    if (!base) base = (DWORD64)mod;
    for (int i = 0; i < n; ++i) *reqs[i].out = ResolveSym(base, reqs[i].mask, reqs[i].n1, reqs[i].n2);
    SymCleanup(proc);
}

// --- Etat partage -----------------------------------------------------------
static PIDLIST_ABSOLUTE g_lastRealFolder = nullptr; // dossier scope de la recherche
static SRWLOCK g_stLock = SRWLOCK_INIT;
static bool g_armSwap = false;        // la prochaine navigation est une recherche
static bool g_searching = false;      // on affiche actuellement nos resultats
static std::wstring g_query;
static bool g_full = false;

// combase : manipulation d'HSTRING (sans dependance de lien).
using FnWCS  = long (WINAPI*)(const wchar_t*, unsigned, void**);
using FnWDS  = long (WINAPI*)(void*);
using FnWGSR = const wchar_t* (WINAPI*)(void*, unsigned*);
static FnWCS  g_WindowsCreateString = nullptr;
static FnWDS  g_WindowsDeleteString = nullptr;
static FnWGSR g_WindowsGetStringRawBuffer = nullptr;

static bool IsFsFolder(PCIDLIST_ABSOLUTE pidl)
{
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &path)) && path)
    { CoTaskMemFree(path); return true; }
    return false;
}

// Vrai si le dossier est DIRECTEMENT sur un volume NTFS local accessible par
// lettre de lecteur (prerequis a la lecture de la MFT de ce volume). Exclut :
//  - partages reseau / lecteurs mappes / UNC (pas de MFT accessible) ;
//  - lecteurs SUBST (pas de volume derriere la lettre) ;
//  - volumes montes dans un dossier, jonctions / liens symboliques vers un
//    autre volume ou le reseau : le chemin FINAL (apres reparse points) doit
//    rester sur le volume de la lettre du chemin ;
//  - tout systeme de fichiers autre que NTFS (FAT, exFAT, ReFS...).
// NB : pas de GetVolumePathNameW, qui SUIT les jonctions (C:\j -> E:\ donne E:\).
static bool IsOnLocalNtfs(PCIDLIST_ABSOLUTE pidl)
{
    PWSTR path = nullptr;
    if (FAILED(SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &path)) || !path) return false;
    bool ok = false;
    wchar_t rootVol[64], fs[MAX_PATH + 1];
    if (((path[0] | 0x20) >= L'a' && (path[0] | 0x20) <= L'z') &&
        path[1] == L':' && path[2] == L'\\')
    {
        const wchar_t root[] = { path[0], L':', L'\\', 0 };
        const UINT type = GetDriveTypeW(root);
        if ((type == DRIVE_FIXED || type == DRIVE_REMOVABLE) &&
            GetVolumeNameForVolumeMountPointW(root, rootVol, ARRAYSIZE(rootVol)))
        {
            HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                // Chemin final sous forme \\?\Volume{GUID}\... : doit commencer
                // par le GUID du volume de la lettre (rootVol finit par '\').
                std::wstring fin(GetFinalPathNameByHandleW(h, nullptr, 0, VOLUME_NAME_GUID), L'\0');
                DWORD n = fin.empty() ? 0
                        : GetFinalPathNameByHandleW(h, fin.data(), (DWORD)fin.size(), VOLUME_NAME_GUID);
                ok = n && n < fin.size() &&
                     _wcsnicmp(fin.c_str(), rootVol, wcslen(rootVol)) == 0 &&
                     GetVolumeInformationByHandleW(h, nullptr, 0, nullptr, nullptr, nullptr, fs, ARRAYSIZE(fs)) &&
                     _wcsicmp(fs, L"NTFS") == 0;
                CloseHandle(h);
            }
        }
    }
    CoTaskMemFree(path);
    return ok;
}

// Fenetre Explorateur au premier plan : renvoie son IShellBrowser (ref +1) et
// le PIDL absolu du dossier courant. L'un ou l'autre peut etre demande.
static bool GetForeground(IShellBrowser** ppBrowser, PIDLIST_ABSOLUTE* ppFolder)
{
    if (ppBrowser) *ppBrowser = nullptr;
    if (ppFolder) *ppFolder = nullptr;
    HWND want = GetAncestor(GetForegroundWindow(), GA_ROOT);
    if (!want) return false;
    IShellWindows* psw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&psw))) || !psw)
        return false;
    long count = 0; psw->get_Count(&count);
    bool ok = false;
    for (long i = 0; i < count && !ok; ++i)
    {
        VARIANT vi{}; vi.vt = VT_I4; vi.lVal = i;
        IDispatch* disp = nullptr;
        if (FAILED(psw->Item(vi, &disp)) || !disp) continue;
        IServiceProvider* sp = nullptr;
        if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&sp))) && sp)
        {
            IShellBrowser* br = nullptr;
            if (SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&br))) && br)
            {
                HWND bh = nullptr;
                if (SUCCEEDED(br->GetWindow(&bh)) && GetAncestor(bh, GA_ROOT) == want)
                {
                    if (ppFolder)
                    {
                        IShellView* view = nullptr;
                        if (SUCCEEDED(br->QueryActiveShellView(&view)) && view)
                        {
                            IFolderView2* fv = nullptr;
                            if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&fv))) && fv)
                            {
                                IPersistFolder2* pf2 = nullptr;
                                if (SUCCEEDED(fv->GetFolder(IID_PPV_ARGS(&pf2))) && pf2)
                                { pf2->GetCurFolder(ppFolder); pf2->Release(); }
                                fv->Release();
                            }
                            view->Release();
                        }
                    }
                    if (ppBrowser) { *ppBrowser = br; br = nullptr; }
                    ok = true;
                }
                if (br) br->Release();
            }
            sp->Release();
        }
        disp->Release();
    }
    psw->Release();
    return ok;
}

// --- Hook 1 : CTabSearchTarget::OnSearchTextNotify --------------------------
// Role : enregistrer la requete et ARMER la substitution. On NE navigue PAS
// et on NE supprime PAS : la navigation native s'execute (elle preserve le
// focus et le contenu de la box).
using FnNotify = HRESULT (WINAPI*)(void*, const wchar_t*, const wchar_t*, int);
static FnNotify g_origNotify = nullptr;
static const int SSN_SUBMIT = 8;

static thread_local bool g_inNotify = false;

static HRESULT WINAPI Detour_Notify(void* self, const wchar_t* text, const wchar_t* scope, int notify)
{
    if (g_inNotify) return g_origNotify(self, text, scope, notify);
    g_inNotify = true;
    const bool full = Options().matchFullName.load();

    if (text && *text)
    {
        SearchNse_SetLiveQuery(text, full);
        AcquireSRWLockExclusive(&g_stLock);
        g_query = text; g_full = full;
        ReleaseSRWLockExclusive(&g_stLock);

        IShellBrowser* br = nullptr; PIDLIST_ABSOLUTE cur = nullptr;
        GetForeground(&br, &cur);

        bool inOurs = cur && SearchNse_IsOurFolder(cur);

        if (inOurs)
        {
            // Deja dans nos resultats : actualiser EN PLACE, puis SUPPRIMER le
            // handler natif (pas de re-navigation -> evite la boucle avec la
            // reinjection de texte) ; la box garde texte + focus.
            if (br) { IShellView* v = nullptr; if (SUCCEEDED(br->QueryActiveShellView(&v)) && v) { v->Refresh(); v->Release(); } }
            if (cur) ILFree(cur);
            if (br) br->Release();
            AcquireSRWLockExclusive(&g_stLock); g_armSwap = false; ReleaseSRWLockExclusive(&g_stLock);
            char b[80]; sprintf_s(b, "refresh in place query='%ls'", text); PLog(b);
            g_inNotify = false;
            return S_OK;
        }

        // Entree en mode resultats : uniquement si le dossier courant est
        // directement sur un volume NTFS local (cf. IsOnLocalNtfs). Sinon
        // (reseau, volume monte, FAT/exFAT/ReFS, bibliotheque, resultats
        // natifs...) on n'arme rien : la recherche native de Windows s'execute.
        const bool ntfs = cur && IsOnLocalNtfs(cur);
        AcquireSRWLockExclusive(&g_stLock);
        if (ntfs)
        {
            // Capturer le scope (vrai dossier) AVANT de liberer 'cur'.
            if (g_lastRealFolder) ILFree(g_lastRealFolder);
            g_lastRealFolder = ILCloneFull(cur);
        }
        g_armSwap = ntfs;
        ReleaseSRWLockExclusive(&g_stLock);
        if (cur) ILFree(cur);
        if (br) br->Release();
        PLog(ntfs ? "arm swap (entering search, NTFS)" : "not NTFS -> native search");
    }
    else
    {
        AcquireSRWLockExclusive(&g_stLock); g_armSwap = false; g_searching = false; ReleaseSRWLockExclusive(&g_stLock);
    }

    HRESULT hr = g_origNotify(self, text, scope, notify);
    g_inNotify = false;
    return hr;
}

// --- Hook 2 : CShellBrowser::_NavigateToPidl --------------------------------
// Role : si on est arme (recherche) et que la cible n'est pas deja la notre,
// substituer le PIDL par celui de notre NSE (scope = dernier vrai dossier).
// Sinon, memoriser le dernier vrai dossier et laisser passer.
using FnNav = HRESULT (WINAPI*)(void*, PCIDLIST_ABSOLUTE, DWORD, DWORD);
static FnNav g_origNav = nullptr;

static HRESULT WINAPI Detour_Nav(void* self, PCIDLIST_ABSOLUTE pidl, DWORD f1, DWORD f2)
{
    bool arm; std::wstring q; bool full; PIDLIST_ABSOLUTE scope = nullptr;
    AcquireSRWLockExclusive(&g_stLock);
    arm = g_armSwap; q = g_query; full = g_full;
    if (arm && g_lastRealFolder) scope = ILCloneFull(g_lastRealFolder);
    if (arm) g_armSwap = false; // une seule substitution par soumission
    ReleaseSRWLockExclusive(&g_stLock);

    { char b[64]; sprintf_s(b, "nav arm=%d scope=%d ours=%d", arm?1:0, scope?1:0,
        SearchNse_IsOurFolder(pidl)?1:0); PLog(b); }

    if (arm && scope && !q.empty() && !SearchNse_IsOurFolder(pidl))
    {
        PIDLIST_ABSOLUTE nse = SearchNse_CreateResultsPidl(scope, q, full);
        ILFree(scope);
        if (nse)
        {
            AcquireSRWLockExclusive(&g_stLock); g_searching = true; ReleaseSRWLockExclusive(&g_stLock);
            char b[80]; sprintf_s(b, "swap -> NSE query='%ls'", q.c_str()); PLog(b);
            HRESULT hr = g_origNav(self, nse, f1, f2);
            ILFree(nse);
            return hr;
        }
    }
    if (scope) ILFree(scope);
    // Navigation normale vers un vrai dossier : fin de session de recherche.
    if (!SearchNse_IsOurFolder(pidl) && IsFsFolder(pidl))
    { AcquireSRWLockExclusive(&g_stLock); g_searching = false; ReleaseSRWLockExclusive(&g_stLock); }
    return g_origNav(self, pidl, f1, f2);
}

// --- Hook 3 : SearchBoxControlVM::SearchText (setter, FileExplorerExtensions)
// Role : la navigation vers nos resultats declenche un "clear" (SearchText vide).
// Tant qu'une recherche est active, on REMPLACE ce vide par notre requete : la
// box conserve donc le texte (et le focus). On laisse passer tout le reste
// (echos XAML de la saisie, vrais clears hors recherche).
using FnSetText = void (WINAPI*)(void* self, void* phstring);
static FnSetText g_origSetText = nullptr;
static thread_local bool g_inSetText = false;
static DWORD64 g_xamlBase = 0, g_xamlEnd = 0;   // Microsoft.UI.Xaml.dll (saisie utilisateur)

static std::wstring g_lastUserText;   // dernier texte saisi (vu via le moteur XAML)

static std::wstring HStrGet(void* phstr)
{
    if (!g_WindowsGetStringRawBuffer || !phstr) return L"";
    void* h = *reinterpret_cast<void**>(phstr);
    unsigned len = 0; const wchar_t* p = g_WindowsGetStringRawBuffer(h, &len);
    return (p && len) ? std::wstring(p, len) : std::wstring();
}

// Vrai si la pile d'appels traverse Microsoft.UI.Xaml.dll (chemin saisie/edition
// utilisateur). Le "clear" de navigation, lui, ne passe pas par XAML.
static bool CalledViaXaml()
{
    if (!g_xamlBase) return false;
    void* frames[20];
    USHORT n = RtlCaptureStackBackTrace(1, 20, frames, nullptr);
    for (USHORT i = 0; i < n; ++i)
    {
        DWORD64 a = (DWORD64)frames[i];
        if (a >= g_xamlBase && a < g_xamlEnd) return true;
    }
    return false;
}

static void WINAPI Detour_SetText(void* self, void* phstr)
{
    if (!g_inSetText)
    {
        std::wstring val = HStrGet(phstr);
        if (!val.empty())
        {
            // Tout texte non-vide = texte utilisateur courant : on memorise.
            AcquireSRWLockExclusive(&g_stLock); g_lastUserText = val; ReleaseSRWLockExclusive(&g_stLock);
        }
        else
        {
            bool viaXaml = CalledViaXaml();
            bool searching; std::wstring last;
            AcquireSRWLockShared(&g_stLock); searching = g_searching; last = g_lastUserText; ReleaseSRWLockShared(&g_stLock);
            if (viaXaml)
            {
                // Effacement utilisateur : accepter (vider g_lastUserText).
                AcquireSRWLockExclusive(&g_stLock); g_lastUserText.clear(); ReleaseSRWLockExclusive(&g_stLock);
            }
            else if (searching && !last.empty() && g_WindowsCreateString)
            {
                // Clear de navigation : remplacer par le dernier texte saisi.
                void* hs = nullptr;
                if (g_WindowsCreateString(last.c_str(), (unsigned)last.size(), &hs) == 0)
                {
                    g_inSetText = true;
                    g_origSetText(self, &hs);
                    g_inSetText = false;
                    if (g_WindowsDeleteString) g_WindowsDeleteString(hs);
                    return;
                }
            }
        }
    }
    g_origSetText(self, phstr);
}

void InstallPipelineHook()
{
    HMODULE ef = nullptr, fee = nullptr;
    for (int i = 0; i < 120 && (!ef || !fee); ++i)
    {
        if (!ef)  ef  = GetModuleHandleW(L"explorerframe.dll");
        if (!fee) fee = GetModuleHandleW(L"FileExplorerExtensions.dll");
        if (!ef || !fee) Sleep(500);
    }
    if (!ef) { PLog("explorerframe.dll not loaded"); return; }

    DWORD64 aNotify = 0, aNav = 0;
    SymReq efReqs[] = {
        { "?OnSearchTextNotify@CTabSearchTarget@@*", "OnSearchTextNotify", "CTabSearchTarget", &aNotify },
        { "?_NavigateToPidl@CShellBrowser@@*",       "_NavigateToPidl@CShellBrowser@@", "",     &aNav },
    };
    ResolveModuleSyms(ef, L"explorerframe.pdb", efReqs, 2);

    DWORD64 aSetText = 0;
    if (fee)
    {
        SymReq feeReqs[] = {
            { "?SearchText@SearchBoxControlVM@*", "SearchText", "SearchBoxControlVM", &aSetText },
        };
        ResolveModuleSyms(fee, L"FileExplorerExtensions.pdb", feeReqs, 1);
    }
    { char b[128]; sprintf_s(b, "RVA notify=0x%llX nav=0x%llX setText=0x%llX",
        (unsigned long long)(aNotify?aNotify-(DWORD64)ef:0), (unsigned long long)(aNav?aNav-(DWORD64)ef:0),
        (unsigned long long)(aSetText&&fee?aSetText-(DWORD64)fee:0)); PLog(b); }
    if (!aNotify || !aNav) { PLog("explorerframe symbol(s) not found"); return; }

    if (HMODULE cb = GetModuleHandleW(L"combase.dll"))
    {
        g_WindowsCreateString       = (FnWCS) GetProcAddress(cb, "WindowsCreateString");
        g_WindowsDeleteString       = (FnWDS) GetProcAddress(cb, "WindowsDeleteString");
        g_WindowsGetStringRawBuffer = (FnWGSR)GetProcAddress(cb, "WindowsGetStringRawBuffer");
    }
    if (HMODULE xaml = GetModuleHandleW(L"Microsoft.UI.Xaml.dll"))
    {
        MODULEINFO xmi{}; if (GetModuleInformation(GetCurrentProcess(), xaml, &xmi, sizeof(xmi)))
        { g_xamlBase = (DWORD64)xaml; g_xamlEnd = g_xamlBase + xmi.SizeOfImage; }
    }

    if (MH_Initialize() != MH_OK) { PLog("MH_Initialize failed"); return; }
    if (MH_CreateHook((LPVOID)aNotify, (LPVOID)&Detour_Notify, (LPVOID*)&g_origNotify) != MH_OK ||
        MH_CreateHook((LPVOID)aNav, (LPVOID)&Detour_Nav, (LPVOID*)&g_origNav) != MH_OK)
    { PLog("MH_CreateHook failed"); return; }
    if (aSetText)
        MH_CreateHook((LPVOID)aSetText, (LPVOID)&Detour_SetText, (LPVOID*)&g_origSetText);
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { PLog("MH_EnableHook failed"); return; }
    PLog("pipeline hooks enabled (OnSearchTextNotify + _NavigateToPidl + SearchText)");
}
