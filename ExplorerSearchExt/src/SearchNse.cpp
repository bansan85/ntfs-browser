// Extension d'espace de noms filtrante. Enveloppe le dossier scope reel,
// ne remplace que EnumObjects (mon algo) et CreateViewObject(IShellView).
// Tout le reste est delegue au dossier reel (PIDL enfants = vrais enfants).
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <new>
#include <string>
#include "SearchNse.h"
#include "Options.h"

#pragma comment(lib, "shlwapi.lib")

// {7C9A2B10-5E43-4F8A-9C21-3D8E1F0A6B44}
extern "C" const CLSID CLSID_ExplorerSearchNse =
{ 0x7c9a2b10, 0x5e43, 0x4f8a, { 0x9c, 0x21, 0x3d, 0x8e, 0x1f, 0x0a, 0x6b, 0x44 } };

extern HINSTANCE g_hinst;

// --------------------------------------------------------------------------
// Encodage du PIDL enfant "donnee" qui porte scope + requete.
// --------------------------------------------------------------------------
static const USHORT NSE_SIG = 0x5358; // 'SX'

#pragma pack(push, 1)
struct NSEITEM
{
    USHORT cb;         // taille de ce SHITEMID (cb compris)
    USHORT sig;        // NSE_SIG
    ULONG  flags;      // bit0 = matchFullName
    ULONG  queryBytes; // octets de la requete (wchar, sans NUL)
    ULONG  scopeBytes; // octets du PIDL scope absolu
    // suit : query (wchars), puis scope pidl (octets)
};
#pragma pack(pop)

// Requete "live" partagee (meme processus que la vue : serveur in-proc).
static SRWLOCK      g_liveLock = SRWLOCK_INIT;
static std::wstring g_liveQuery;
static bool         g_liveFull = false;
static bool         g_liveActive = false;
// Incremente a chaque changement de requete : un enumerateur en cours pour une
// requete perimee s'arrete aussitot (DefView differe le Refresh demande par la
// frappe suivante jusqu'a la fin de l'enumeration en cours).
static volatile LONG g_liveGen = 0;

void SearchNse_SetLiveQuery(std::wstring const& query, bool matchFullName)
{
    AcquireSRWLockExclusive(&g_liveLock);
    if (!g_liveActive || g_liveQuery != query || g_liveFull != matchFullName)
        InterlockedIncrement(&g_liveGen);
    g_liveQuery = query; g_liveFull = matchFullName; g_liveActive = true;
    ReleaseSRWLockExclusive(&g_liveLock);
}

static const NSEITEM* AsNseItem(PCUIDLIST_RELATIVE pidl)
{
    if (!pidl) return nullptr;
    auto* p = reinterpret_cast<const NSEITEM*>(pidl);
    if (p->cb < sizeof(NSEITEM)) return nullptr;
    if (p->sig != NSE_SIG) return nullptr;
    return p;
}

static UINT PidlAbsBytes(PCIDLIST_ABSOLUTE p) // octets jusqu'au terminateur inclus
{
    return static_cast<UINT>(ILGetSize(p));
}

// Regitem racine du bureau pour notre CLSID : { cb=0x14, 0x1F, ordre, GUID16 }.
// Construit directement (le parse "::{CLSID}" n'accepte que les membres de
// namespace ; le BIND, lui, ne requiert que l'enregistrement ShellFolder).
#pragma pack(push, 1)
struct REGITEM { USHORT cb; BYTE type; BYTE order; GUID guid; };
#pragma pack(pop)

PIDLIST_ABSOLUTE SearchNse_CreateResultsPidl(PCIDLIST_ABSOLUTE scopeAbs,
                                             std::wstring const& query,
                                             bool matchFullName)
{
    const ULONG qBytes = static_cast<ULONG>(query.size() * sizeof(wchar_t));
    const ULONG sBytes = scopeAbs ? PidlAbsBytes(scopeAbs) : 0;
    const size_t rootBytes = sizeof(REGITEM);                         // item 1
    const size_t dataBytes = sizeof(NSEITEM) + qBytes + sBytes;       // item 2
    const size_t idlBytes  = rootBytes + dataBytes + sizeof(USHORT);  // + terminateur

    auto* buf = static_cast<BYTE*>(CoTaskMemAlloc(idlBytes));
    if (!buf) return nullptr;
    ZeroMemory(buf, idlBytes);

    auto* root = reinterpret_cast<REGITEM*>(buf);
    root->cb = static_cast<USHORT>(sizeof(REGITEM));
    root->type = 0x1F;   // regitem (dossier CLSID du bureau)
    root->order = 0x80;  // groupe de tri (cosmetique)
    root->guid = CLSID_ExplorerSearchNse;

    auto* it = reinterpret_cast<NSEITEM*>(buf + rootBytes);
    it->cb = static_cast<USHORT>(dataBytes);
    it->sig = NSE_SIG;
    it->flags = matchFullName ? 1u : 0u;
    it->queryBytes = qBytes;
    it->scopeBytes = sBytes;
    memcpy(buf + rootBytes + sizeof(NSEITEM), query.data(), qBytes);
    if (sBytes) memcpy(buf + rootBytes + sizeof(NSEITEM) + qBytes, scopeAbs, sBytes);
    // terminateur (USHORT 0) deja a zero.

    return reinterpret_cast<PIDLIST_ABSOLUTE>(buf); // a liberer par ILFree/CoTaskMemFree
}

bool SearchNse_IsOurFolder(PCIDLIST_ABSOLUTE pidl)
{
    if (!pidl) return false;
    PCUITEMID_CHILD last = ILFindLastID(pidl);
    return AsNseItem(reinterpret_cast<PCUIDLIST_RELATIVE>(last)) != nullptr;
}

// --------------------------------------------------------------------------
// Enumerateur filtrant EN FLUX : la recherche se fait dans Next(), au fil de
// l'enumeration du dossier scope, au lieu d'etre terminee avant de rendre
// l'enumerateur. DefView appelle Next() sur son thread producteur et publie
// ce qu'il a recu vers la vue par lots (Windows.Storage CEnumTask : un lot
// toutes les 100 ms, +100 ms par lot, plafond 2 s ; reveil anticipe au-dela
// de 50 items en attente) -> affichage progressif, comme la recherche native.
// Annulation : (1) DefView pose son site sur l'enumerateur (IUnknown_SetSite) ;
// son IQueryContinue passe a S_FALSE quand l'enumeration est abandonnee
// (navigation, fermeture). (2) Une nouvelle requete (frappe) change g_liveGen :
// on termine tout de suite, ce qui libere le Refresh differe par DefView.
// --------------------------------------------------------------------------
class CStreamEnum : public IEnumIDList, public IObjectWithSite
{
    LONG m_ref = 1;
    IShellFolder* m_scope;          // dossier reel (ref)
    IEnumIDList* m_src;             // enumeration du dossier reel (ref) ; null = fini
    std::wstring m_query;
    bool m_matchFull;
    IUnknown* m_site = nullptr;
    IQueryContinue* m_qc = nullptr;
    ULONG m_scanned = 0;
    LONG m_gen;                     // generation de la requete a la creation

    static const ULONG CANCEL_CHECK_EVERY = 32; // items parcourus entre deux QueryContinue

    bool Cancelled()
    {
        if (g_liveGen != m_gen) return true; // requete perimee
        return m_qc && (++m_scanned % CANCEL_CHECK_EVERY) == 0 && m_qc->QueryContinue() != S_OK;
    }

    bool Matches(PCUITEMID_CHILD child)
    {
        STRRET sr{}; std::wstring name;
        if (SUCCEEDED(m_scope->GetDisplayNameOf(child, SHGDN_INFOLDER | SHGDN_FORPARSING, &sr)))
        {
            PWSTR psz = nullptr;
            if (SUCCEEDED(StrRetToStrW(&sr, child, &psz)) && psz) { name = psz; CoTaskMemFree(psz); }
        }
        return !name.empty() && (m_matchFull
            ? (_wcsicmp(name.c_str(), m_query.c_str()) == 0)
            : (StrStrIW(name.c_str(), m_query.c_str()) != nullptr));
    }

    void Finish() { if (m_src) { m_src->Release(); m_src = nullptr; } }

public:
    // Prend une reference sur 'scope' ; ADOPTE 'src' (peut etre null : vide).
    CStreamEnum(IShellFolder* scope, IEnumIDList* src, std::wstring query, bool matchFull, LONG gen)
        : m_scope(scope), m_src(src), m_query(std::move(query)), m_matchFull(matchFull), m_gen(gen)
    { m_scope->AddRef(); }
    ~CStreamEnum()
    {
        Finish();
        if (m_qc) m_qc->Release();
        if (m_site) m_site->Release();
        m_scope->Release();
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IEnumIDList)
            { *ppv = static_cast<IEnumIDList*>(this); AddRef(); return S_OK; }
        if (riid == IID_IObjectWithSite)
            { *ppv = static_cast<IObjectWithSite*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    IFACEMETHODIMP_(ULONG) Release() override
    { LONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }

    // --- IEnumIDList : chaque appel parcourt le dossier jusqu'a 'celt' hits ---
    IFACEMETHODIMP Next(ULONG celt, PITEMID_CHILD* rgelt, ULONG* fetched) override
    {
        ULONG got = 0;
        while (got < celt && m_src)
        {
            if (Cancelled()) { Finish(); break; }
            PITEMID_CHILD child = nullptr; ULONG f = 0;
            if (m_src->Next(1, &child, &f) != S_OK || f != 1) { Finish(); break; }
            if (Matches(child)) rgelt[got++] = child; // l'appelant devient proprietaire
            else CoTaskMemFree(child);
        }
        if (fetched) *fetched = got;
        return got == celt ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Skip(ULONG celt) override
    {
        for (ULONG i = 0; i < celt; ++i)
        {
            PITEMID_CHILD c = nullptr;
            if (Next(1, &c, nullptr) != S_OK) return S_FALSE;
            CoTaskMemFree(c);
        }
        return S_OK;
    }
    IFACEMETHODIMP Reset() override { return E_NOTIMPL; }   // flux : pas de retour arriere
    IFACEMETHODIMP Clone(IEnumIDList** ppenum) override { *ppenum = nullptr; return E_NOTIMPL; }

    // --- IObjectWithSite : site = tache d'enumeration de DefView ---
    IFACEMETHODIMP SetSite(IUnknown* site) override
    {
        if (m_qc) { m_qc->Release(); m_qc = nullptr; }
        if (m_site) m_site->Release();
        m_site = site;
        if (m_site)
        {
            m_site->AddRef();
            if (FAILED(m_site->QueryInterface(IID_PPV_ARGS(&m_qc))))
                IUnknown_QueryService(m_site, IID_IQueryContinue, IID_PPV_ARGS(&m_qc));
        }
        return S_OK;
    }
    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) override
    {
        *ppv = nullptr;
        return m_site ? m_site->QueryInterface(riid, ppv) : E_FAIL;
    }
};

// --------------------------------------------------------------------------
// Le dossier : racine (bind waypoint) ou configure (scope+requete).
// --------------------------------------------------------------------------
class CSearchFolder : public IShellFolder2, public IPersistFolder2
{
    LONG m_ref = 1;
    PIDLIST_ABSOLUTE m_pidlAbs = nullptr;   // PIDL absolu de CE dossier
    PIDLIST_ABSOLUTE m_scopeAbs = nullptr;  // dossier reel a chercher (configure)
    std::wstring m_query;
    bool m_matchFull = false;
    bool m_configured = false;
    IShellFolder* m_scope = nullptr;        // dossier reel (lie paresseusement)

    IShellFolder* Scope()
    {
        if (!m_scope && m_scopeAbs)
            SHBindToObject(nullptr, m_scopeAbs, nullptr, IID_PPV_ARGS(&m_scope));
        return m_scope;
    }

public:
    CSearchFolder() {}
    ~CSearchFolder()
    {
        if (m_pidlAbs) ILFree(m_pidlAbs);
        if (m_scopeAbs) ILFree(m_scopeAbs);
        if (m_scope) m_scope->Release();
    }

    // --- IUnknown ---
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IShellFolder || riid == IID_IShellFolder2)
            { *ppv = static_cast<IShellFolder2*>(this); AddRef(); return S_OK; }
        if (riid == IID_IPersist || riid == IID_IPersistFolder ||
            riid == IID_IPersistFolder2)
            { *ppv = static_cast<IPersistFolder2*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    IFACEMETHODIMP_(ULONG) Release() override
    { LONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }

    // --- IPersist / IPersistFolder / IPersistFolder2 ---
    IFACEMETHODIMP GetClassID(CLSID* p) override { *p = CLSID_ExplorerSearchNse; return S_OK; }
    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE pidl) override
    {
        if (m_pidlAbs) ILFree(m_pidlAbs);
        m_pidlAbs = ILCloneFull(pidl);
        return S_OK;
    }
    IFACEMETHODIMP GetCurFolder(PIDLIST_ABSOLUTE* ppidl) override
    { *ppidl = m_pidlAbs ? ILCloneFull(m_pidlAbs) : nullptr; return *ppidl ? S_OK : E_FAIL; }

    // Cree une instance configuree a partir du PIDL donnee 'it'.
    HRESULT MakeConfigured(const NSEITEM* it, PCUIDLIST_RELATIVE dataPidl, REFIID riid, void** ppv)
    {
        auto* f = new (std::nothrow) CSearchFolder();
        if (!f) return E_OUTOFMEMORY;
        f->m_configured = true;
        f->m_matchFull = (it->flags & 1u) != 0;
        const BYTE* base = reinterpret_cast<const BYTE*>(it);
        f->m_query.assign(reinterpret_cast<const wchar_t*>(base + sizeof(NSEITEM)),
                          it->queryBytes / sizeof(wchar_t));
        if (it->scopeBytes)
        {
            auto* sp = static_cast<BYTE*>(CoTaskMemAlloc(it->scopeBytes));
            if (sp)
            {
                memcpy(sp, base + sizeof(NSEITEM) + it->queryBytes, it->scopeBytes);
                f->m_scopeAbs = reinterpret_cast<PIDLIST_ABSOLUTE>(sp);
            }
        }
        // PIDL absolu de l'instance configuree = CE pidl + dataPidl.
        f->m_pidlAbs = m_pidlAbs ? ILCombine(m_pidlAbs, dataPidl) : nullptr;
        HRESULT hr = f->QueryInterface(riid, ppv);
        f->Release();
        return hr;
    }

    // --- IShellFolder ---
    IFACEMETHODIMP ParseDisplayName(HWND hwnd, IBindCtx* pbc, LPWSTR disp, ULONG* eaten,
                                    PIDLIST_RELATIVE* ppidl, ULONG* attr) override
    { IShellFolder* s = Scope(); return s ? s->ParseDisplayName(hwnd, pbc, disp, eaten, ppidl, attr) : E_NOTIMPL; }

    IFACEMETHODIMP EnumObjects(HWND hwnd, SHCONTF flags, IEnumIDList** ppenum) override
    {
        *ppenum = nullptr;
        if (!m_configured) { return S_FALSE; } // racine : rien a lister
        IShellFolder* s = Scope();
        if (!s) return E_FAIL;

        // Requete effective : la "live" (actualisation en place) si active,
        // sinon celle encodee dans le PIDL de cette instance.
        std::wstring query; bool matchFull; LONG gen;
        AcquireSRWLockShared(&g_liveLock);
        if (g_liveActive) { query = g_liveQuery; matchFull = g_liveFull; }
        else              { query = m_query;     matchFull = m_matchFull; }
        gen = g_liveGen;
        ReleaseSRWLockShared(&g_liveLock);

        // Requete vide : enumerateur vide (rien a montrer).
        IEnumIDList* en = nullptr;
        if (!query.empty())
        {
            HRESULT hr = s->EnumObjects(hwnd,
                SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &en);
            if (FAILED(hr)) return hr;
        }
        // Retour IMMEDIAT : le filtrage se fait au fil de Next() (affichage progressif).
        auto* e = new (std::nothrow) CStreamEnum(s, en, std::move(query), matchFull, gen);
        if (!e) { if (en) en->Release(); return E_OUTOFMEMORY; }
        *ppenum = e;
        return S_OK;
    }

    IFACEMETHODIMP BindToObject(PCUIDLIST_RELATIVE pidl, IBindCtx* pbc, REFIID riid, void** ppv) override
    {
        if (const NSEITEM* it = AsNseItem(pidl))
            return MakeConfigured(it, pidl, riid, ppv); // -> instance configuree
        IShellFolder* s = Scope();
        return s ? s->BindToObject(pidl, pbc, riid, ppv) : E_NOTIMPL;
    }
    IFACEMETHODIMP BindToStorage(PCUIDLIST_RELATIVE pidl, IBindCtx* pbc, REFIID riid, void** ppv) override
    { IShellFolder* s = Scope(); return s ? s->BindToStorage(pidl, pbc, riid, ppv) : E_NOTIMPL; }

    IFACEMETHODIMP CompareIDs(LPARAM lp, PCUIDLIST_RELATIVE a, PCUIDLIST_RELATIVE b) override
    { IShellFolder* s = Scope(); return s ? s->CompareIDs(lp, a, b) : E_NOTIMPL; }

    IFACEMETHODIMP CreateViewObject(HWND hwnd, REFIID riid, void** ppv) override
    {
        *ppv = nullptr;
        if (riid == IID_IShellView)
        {
            SFV_CREATE csfv{ sizeof(csfv) };
            csfv.pshf = static_cast<IShellFolder*>(this);
            return SHCreateShellFolderView(&csfv, reinterpret_cast<IShellView**>(ppv));
        }
        IShellFolder* s = Scope();
        return s ? s->CreateViewObject(hwnd, riid, ppv) : E_NOINTERFACE;
    }

    IFACEMETHODIMP GetAttributesOf(UINT c, PCUITEMID_CHILD_ARRAY a, SFGAOF* inout) override
    {
        IShellFolder* s = Scope();
        if (s && c) return s->GetAttributesOf(c, a, inout);
        // racine : dossier navigable
        *inout &= (SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_BROWSABLE | SFGAO_FILESYSANCESTOR);
        *inout |= SFGAO_FOLDER;
        return S_OK;
    }

    IFACEMETHODIMP GetUIObjectOf(HWND hwnd, UINT c, PCUITEMID_CHILD_ARRAY a, REFIID riid,
                                 UINT* r, void** ppv) override
    { IShellFolder* s = Scope(); return s ? s->GetUIObjectOf(hwnd, c, a, riid, r, ppv) : E_NOINTERFACE; }

    IFACEMETHODIMP GetDisplayNameOf(PCUITEMID_CHILD pidl, SHGDNF flags, STRRET* name) override
    {
        if (const NSEITEM* it = AsNseItem(pidl))
        {
            const BYTE* base = reinterpret_cast<const BYTE*>(it);
            std::wstring q(reinterpret_cast<const wchar_t*>(base + sizeof(NSEITEM)),
                           it->queryBytes / sizeof(wchar_t));
            std::wstring s;
            if (flags & SHGDN_FORADDRESSBAR)
            {
                // Nom "barre d'adresse" au format search-ms canonique : l'Explorateur
                // reconnait un emplacement de RECHERCHE (IsSearchLocation teste le
                // prefixe) ET en derive la requete affichee dans la box (query=...).
                // search-ms minimal et bien forme (pas de caracteres non encodes)
                // pour que le controleur WinUI y lise la requete et garde le texte.
                s = L"search-ms:query=" + q;
            }
            else s = L"Resultats : " + q;
            name->uType = STRRET_WSTR;
            SHStrDupW(s.c_str(), &name->pOleStr);
            return S_OK;
        }
        IShellFolder* sc = Scope();
        return sc ? sc->GetDisplayNameOf(pidl, flags, name) : E_NOTIMPL;
    }

    IFACEMETHODIMP SetNameOf(HWND hwnd, PCUITEMID_CHILD pidl, LPCWSTR nm, SHGDNF f,
                             PITEMID_CHILD* out) override
    { IShellFolder* s = Scope(); return s ? s->SetNameOf(hwnd, pidl, nm, f, out) : E_NOTIMPL; }

    // --- IShellFolder2 : delegation au scope si possible ---
    IShellFolder2* Scope2()
    {
        IShellFolder* s = Scope(); if (!s) return nullptr;
        IShellFolder2* s2 = nullptr; s->QueryInterface(IID_PPV_ARGS(&s2)); return s2;
    }
    IFACEMETHODIMP GetDefaultSearchGUID(GUID* p) override
    { IShellFolder2* s = Scope2(); HRESULT hr = s ? s->GetDefaultSearchGUID(p) : E_NOTIMPL; if (s) s->Release(); return hr; }
    IFACEMETHODIMP EnumSearches(IEnumExtraSearch** p) override
    { if (p) *p = nullptr; return E_NOTIMPL; }
    IFACEMETHODIMP GetDefaultColumn(DWORD d, ULONG* s, ULONG* o) override
    { IShellFolder2* z = Scope2(); HRESULT hr = z ? z->GetDefaultColumn(d, s, o) : E_NOTIMPL; if (z) z->Release(); return hr; }
    IFACEMETHODIMP GetDefaultColumnState(UINT col, SHCOLSTATEF* f) override
    { IShellFolder2* z = Scope2(); HRESULT hr = z ? z->GetDefaultColumnState(col, f) : E_NOTIMPL; if (z) z->Release(); return hr; }
    IFACEMETHODIMP GetDetailsEx(PCUITEMID_CHILD pidl, const SHCOLUMNID* id, VARIANT* v) override
    { IShellFolder2* z = Scope2(); HRESULT hr = z ? z->GetDetailsEx(pidl, id, v) : E_NOTIMPL; if (z) z->Release(); return hr; }
    IFACEMETHODIMP GetDetailsOf(PCUITEMID_CHILD pidl, UINT col, SHELLDETAILS* d) override
    { IShellFolder2* z = Scope2(); HRESULT hr = z ? z->GetDetailsOf(pidl, col, d) : E_NOTIMPL; if (z) z->Release(); return hr; }
    IFACEMETHODIMP MapColumnToSCID(UINT col, SHCOLUMNID* id) override
    { IShellFolder2* z = Scope2(); HRESULT hr = z ? z->MapColumnToSCID(col, id) : E_NOTIMPL; if (z) z->Release(); return hr; }
};

// --------------------------------------------------------------------------
// Fabrique de classe.
// --------------------------------------------------------------------------
class CFactory : public IClassFactory
{
    LONG m_ref = 1;
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    IFACEMETHODIMP_(ULONG) Release() override
    { LONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* f = new (std::nothrow) CSearchFolder();
        if (!f) return E_OUTOFMEMORY;
        HRESULT hr = f->QueryInterface(riid, ppv);
        f->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL) override { return S_OK; }
};

HRESULT SearchNse_GetClassObject(REFIID riid, void** ppv)
{
    auto* f = new (std::nothrow) CFactory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

// --------------------------------------------------------------------------
// Enregistrement permanent sous HKCU\Software\Classes\CLSID\{...}.
// --------------------------------------------------------------------------
static LONG SetKey(HKEY root, const wchar_t* sub, const wchar_t* val, const wchar_t* data)
{
    HKEY k; LONG r = RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(k, val, 0, REG_SZ, reinterpret_cast<const BYTE*>(data),
                       static_cast<DWORD>((wcslen(data) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r;
}

HRESULT SearchNse_RegisterServer()
{
    wchar_t clsid[64]; StringFromGUID2(CLSID_ExplorerSearchNse, clsid, 64);
    wchar_t dll[MAX_PATH]; GetModuleFileNameW(g_hinst, dll, MAX_PATH);

    std::wstring base = std::wstring(L"Software\\Classes\\CLSID\\") + clsid;
    SetKey(HKEY_CURRENT_USER, base.c_str(), nullptr, L"Explorer Search (algo perso)");
    std::wstring inproc = base + L"\\InprocServer32";
    SetKey(HKEY_CURRENT_USER, inproc.c_str(), nullptr, dll);
    SetKey(HKEY_CURRENT_USER, inproc.c_str(), L"ThreadingModel", L"Apartment");
    std::wstring sf = base + L"\\ShellFolder";
    // Attributs : dossier navigable.
    HKEY k; if (RegCreateKeyExW(HKEY_CURRENT_USER, sf.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS)
    {
        DWORD attr = SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_BROWSABLE;
        RegSetValueExW(k, L"Attributes", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&attr), sizeof(attr));
        RegCloseKey(k);
    }
    return S_OK;
}

HRESULT SearchNse_UnregisterServer()
{
    wchar_t clsid[64]; StringFromGUID2(CLSID_ExplorerSearchNse, clsid, 64);
    std::wstring base = std::wstring(L"Software\\Classes\\CLSID\\") + clsid;
    SHDeleteKeyW(HKEY_CURRENT_USER, base.c_str());
    return S_OK;
}

// Export de test (validation hors Explorateur) : construit le PIDL de resultats.
extern "C" __declspec(dllexport) PIDLIST_ABSOLUTE __stdcall
TestCreateResultsPidl(PCIDLIST_ABSOLUTE scope, const wchar_t* q, int full)
{
    return SearchNse_CreateResultsPidl(scope, q ? q : L"", full != 0);
}
