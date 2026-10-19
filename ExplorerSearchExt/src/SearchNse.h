#pragma once
#include <windows.h>
#include <shlobj.h>
#include <string>

// Extension d'espace de noms (NSE) "resultats de recherche" minimale et
// FILTRANTE : elle enveloppe le dossier scope reel et ne remplace que
// l'enumeration (mon algo : dossier courant, sans recursion). Tout le reste
// (icones, menus, colonnes, ouverture) est delegue au dossier reel, car les
// PIDL enfants que j'enumere SONT les vrais enfants du dossier scope.

// CLSID de l'extension (enregistre en permanence sous HKCU).
// {7C9A2B10-5E43-4F8A-9C21-3D8E1F0A6B44}
extern "C" const CLSID CLSID_ExplorerSearchNse;

// Fabrique de classe exposee par DllGetClassObject.
HRESULT SearchNse_GetClassObject(REFIID riid, void** ppv);

// Construit un PIDL ABSOLU navigable vers la NSE, encodant le dossier scope
// (ou chercher) et la requete. A liberer par CoTaskMemFree/ILFree.
// scopeAbs : PIDL absolu du dossier courant ; query : terme ; matchFullName : option.
PIDLIST_ABSOLUTE SearchNse_CreateResultsPidl(PCIDLIST_ABSOLUTE scopeAbs,
                                             std::wstring const& query,
                                             bool matchFullName);

// Enregistrement permanent du CLSID (HKCU\Software\Classes\CLSID\{...}).
HRESULT SearchNse_RegisterServer();
HRESULT SearchNse_UnregisterServer();

// Requete "live" partagee : permet d'actualiser la vue en place (Refresh) sans
// re-naviguer. EnumObjects l'utilise en priorite tant qu'elle est active.
void SearchNse_SetLiveQuery(std::wstring const& query, bool matchFullName);

// Vrai si 'pidl' designe notre extension (dernier SHITEMID = item NSE).
bool SearchNse_IsOurFolder(PCIDLIST_ABSOLUTE pidl);
