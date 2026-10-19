cmake_minimum_required(VERSION 3.26)

# ---------------------------------------------------------------------------
# ExplorerSearchExt : la box de recherche NATIVE de l'Explorateur Windows 11
# pilote MON algorithme de recherche.
#
# Principe (pur Win32/COM, DLL injectee dans explorer.exe) :
#   - hook de CTabSearchTarget::OnSearchTextNotify + CShellBrowser::_NavigateToPidl
#     (explorerframe.dll, adresses resolues au runtime via le PDB) ;
#   - a la frappe, on substitue la cible de navigation par une extension
#     d'espace de noms (NSE) filtrante qui enumere MES resultats (ici : dossier
#     courant, sans recursion) et s'affiche dans une vraie vue Explorateur.
#
# Build :
#   cmake -S . -B build -G "Ninja Multi-Config"
#   cmake --build build --config RelWithDebInfo
# Artefacts (build/out/<config>/) : ExplorerSearchExt.dll + ExplorerSearchInject.exe
# ---------------------------------------------------------------------------

project(ExplorerSearchExt LANGUAGES CXX C)

if(NOT WIN32)
  message(FATAL_ERROR "Projet Windows uniquement.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Compiler en x64 (explorer.exe est 64 bits).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(_common_defs WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# MinHook (trampoline x64) : lib C separee.
add_library(minhook STATIC
  vendor/minhook/src/buffer.c
  vendor/minhook/src/hook.c
  vendor/minhook/src/trampoline.c
  vendor/minhook/src/hde/hde32.c
  vendor/minhook/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC vendor/minhook/include)

# La DLL injectee : hook du pipeline + extension d'espace de noms filtrante.
add_library(ExplorerSearchExt SHARED
  src/dllmain.cpp
  src/PipelineHook.cpp
  src/SearchNse.cpp
  src/ExplorerSearchExt.def
)
target_compile_definitions(ExplorerSearchExt PRIVATE ${_common_defs})
target_link_libraries(ExplorerSearchExt PRIVATE
  minhook shlwapi shell32 ole32 oleaut32 user32 dbghelp psapi)

# L'injecteur.
add_executable(ExplorerSearchInject src/inject.cpp)
target_compile_definitions(ExplorerSearchInject PRIVATE ${_common_defs})
target_link_libraries(ExplorerSearchInject PRIVATE ole32 oleaut32 shlwapi user32)

set_target_properties(ExplorerSearchExt ExplorerSearchInject PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)
