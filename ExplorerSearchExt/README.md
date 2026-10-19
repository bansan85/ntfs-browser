# ExplorerSearchExt

La **box de recherche native** de l'Explorateur Windows 11 pilote **mon propre
algorithme** de recherche, et les résultats s'affichent dans une **vraie vue
Explorateur**. Exemple fourni : recherche dans le **dossier courant, sans
récursion** (ce qui prouve que c'est bien mon algo — la recherche Windows, elle,
est récursive et indexée) — avec une option « nom complet » vs « sous-chaîne ».

Aucune GUI externe, aucune surcouche : la recherche se déclenche depuis le champ
natif, en temps réel.

## Build

Prérequis : Visual Studio (MSVC x64), Windows SDK, CMake ≥ 3.26.

```powershell
# Developer PowerShell for VS, x64
cmake -S . -B build -G "Ninja Multi-Config"
cmake --build build --config RelWithDebInfo
```

Artefacts : `build/out/RelWithDebInfo/ExplorerSearchExt.dll` +
`ExplorerSearchInject.exe`.

## Utilisation

```powershell
build\out\RelWithDebInfo\ExplorerSearchInject.exe
```

L'injecteur charge la DLL dans le(s) processus `explorer.exe` qui hébergent les
fenêtres de l'Explorateur. Ensuite, dans une fenêtre, tape dans le champ de
recherche natif : la vue bascule en « Résultats » et n'affiche que les éléments
du dossier courant (sans récursion) dont le nom correspond — via **mon**
algorithme, en temps réel.

Le hook disparaît au redémarrage d'`explorer.exe` ; relancer l'injecteur pour le
réinstaller.

## Fonctionnement (pur Win32/COM)

1. **Hooks** (MinHook) dans `explorerframe.dll`, adresses résolues au runtime via
   le PDB (DbgHelp, `%TEMP%\SymbolCache`) :
   - `CTabSearchTarget::OnSearchTextNotify` — la box WinUI soumet la requête (à
     chaque frappe). On y enregistre la requête et, selon le cas, on actualise la
     vue en place ou on arme la substitution.
   - `CShellBrowser::_NavigateToPidl` — à l'entrée en recherche, on **substitue**
     le PIDL de destination par celui de notre extension. La navigation reste
     celle de l'Explorateur (focus/box préservés).
   - `SearchBoxControlVM::SearchText(hstring)` (FileExplorerExtensions.dll) — le
     texte de la box est piloté par le contrôleur de navigation WinUI, remis à
     vide à chaque navigation. On **réinjecte** le dernier texte tapé lors de ce
     « clear » de navigation (pour que le champ conserve « k » → « ke » → …),
     tout en laissant passer un vrai effacement utilisateur (détecté quand
     l'appel vient de `Microsoft.UI.Xaml.dll`, via la pile d'appels).
2. **Extension d'espace de noms filtrante** (`SearchNse.cpp`) : un `IShellFolder2`
   qui **enveloppe le dossier scope réel** et délègue tout (icônes, menus,
   colonnes, ouverture) *sauf* `EnumObjects`, qui applique mon filtre
   (sous-chaîne `StrStrIW`, ou nom complet `_wcsicmp`). La vue est créée par
   `SHCreateShellFolderView`.
   **Affichage progressif** : `EnumObjects` rend tout de suite un énumérateur
   *en flux* (`CStreamEnum`) qui filtre au fil de `Next()`. DefView appelle
   `Next()` sur son thread producteur et publie les résultats par lots, avec la
   même cadence que la recherche native (Windows.Storage `CEnumTask` : un lot
   toutes les 100 ms, puis +100 ms par lot, plafond 2 s ; réveil anticipé
   au-delà de 50 items en attente). Annulation : `IQueryContinue` du site posé
   par DefView (navigation/fermeture), et numéro de génération de la requête
   (une frappe interrompt l'énumération périmée, sinon DefView différerait le
   `Refresh` jusqu'à sa fin). S'enregistre une fois sous
   `HKCU\Software\Classes\CLSID\{...}` (COM standard : le shell instancie un
   folder par CLSID).
3. **Reconnaissance « recherche »** : le nom `SIGDN_PARENTRELATIVEFORADDRESSBAR`
   de l'extension commence par `search-ms:` — critère exact testé par
   `SearchBoxControlVM::IsSearchLocation` (FileExplorerExtensions.dll) — pour que
   l'Explorateur garde la box en mode recherche (texte + focus) pendant la saisie.
4. **PIDL navigable** construit manuellement : regitem bureau (`0x1F` + GUID)
   suivi d'un item encodant *(dossier scope + requête)*. Le parse `::{CLSID}` est
   réservé aux membres de namespace, mais le *bind* ne requiert que
   l'enregistrement — d'où la construction directe.

## Étendre l'algorithme

Toute la correspondance est dans `CStreamEnum::Matches` / `CStreamEnum::Next`
([src/SearchNse.cpp](src/SearchNse.cpp)) ; un algorithme lent (contenu, regex…)
s'affiche donc au fil de l'eau sans rien changer d'autre. Remplacer le test de nom par n'importe
quelle logique (regex, contenu, fichiers effacés…) : le reste (vue native,
intégration box) est inchangé.

## Limites

- x64 uniquement. Les adresses dans `explorerframe.dll` sont re-résolues à chaque
  build via le PDB (présent dans `%TEMP%\SymbolCache`) — robuste mais dépend du
  PDB.
- Projet d'exemple : non signé.

## Historique (voie XAML abandonnée)

Une première voie visait à injecter l'UI *dans* le champ via l'API de diagnostic
visuel XAML (`InitializeXamlDiagnosticsEx`) : elle provoque un `__fastfail` dans
le XAML système (`Windows.UI.Xaml.dll`) avant le chargement du TAP (confirmé au
débogueur Frida). Abandonnée au profit du hook du pipeline ci-dessus.
