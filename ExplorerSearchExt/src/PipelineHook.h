#pragma once
// Hook du pipeline de recherche natif : intercepte la soumission de recherche
// de la box WinUI (via CTabSearchTarget::OnSearchTextNotify dans
// explorerframe.dll) et navigue vers notre extension d'espace de noms, dont
// l'enumeration = notre algo (dossier courant, sans recursion).
void InstallPipelineHook();
