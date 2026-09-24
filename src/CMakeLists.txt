set(${PROJECT_NAME}_SRC
    "data/file-record-header.cpp"
    "attr-base.cpp"
    "attr-bitmap.cpp"
    "attr-file-name.cpp"
    "attr-index-alloc.cpp"
    "attr-index-root.cpp"
    "attr-list.cpp"
    "attr-non-resident.cpp"
    "attr-resident.cpp"
    "attr-std-info.cpp"
    "attr-vol-info.cpp"
    "attr-vol-name.cpp"
    "efs/fek.cpp"
    "file-record.cpp"
    "filename.cpp"
    "file-reader.cpp"
    "index-block.cpp"
    "index-entry.cpp"
    "log.cpp"
    "ntfs-volume.cpp"
    "utf.cpp")

if(NTFS_BROWSER_ENABLE_DECOMPRESSION)
  list(APPEND ${PROJECT_NAME}_SRC "lznt1/decompress.cpp")
endif()

if(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP)
  list(APPEND ${PROJECT_NAME}_SRC "efs/cipher-cryptopp.cpp")
endif()

# The backend-agnostic EFS machinery beyond fek.cpp - which key-provider-win.cpp
# needs unconditionally on Windows, so it stays in the base list above - is only
# needed, and only compiles, when at least one backend is compiled in.
# NTFS_BROWSER_EFS_MASTER (set in the root CMakeLists.txt) is that condition.
if(NTFS_BROWSER_EFS_MASTER)
  list(APPEND ${PROJECT_NAME}_SRC "efs/efs-context.cpp" "efs/efs-stream.cpp"
       "efs/efs.cpp")
endif()

if(WIN32)
  list(APPEND ${PROJECT_NAME}_SRC "win32-disk-reader.cpp"
       "efs/key-provider-win.cpp")
  if(NTFS_BROWSER_ENABLE_EFS_BCRYPT)
    list(APPEND ${PROJECT_NAME}_SRC "efs/cipher-bcrypt.cpp")
  endif()
endif()

set(${PROJECT_NAME}_H
    "../include/ntfs-browser/data/attr-defines.h"
    "../include/ntfs-browser/data/attr-header-common.h"
    "../include/ntfs-browser/data/attr-type.h"
    "../include/ntfs-browser/attr-base.h"
    "../include/ntfs-browser/disk-reader.h"
    "../include/ntfs-browser/efs.h"
    "../include/ntfs-browser/file-reader.h"
    "../include/ntfs-browser/file-record.h"
    "../include/ntfs-browser/index-entry.h"
    "../include/ntfs-browser/log.h"
    "../include/ntfs-browser/mask.h"
    "../include/ntfs-browser/mft-idx.h"
    "../include/ntfs-browser/ntfs-volume.h"
    "../include/ntfs-browser/strategy.h"
    "../include/ntfs-browser/win32-disk-reader.h"
    "../include/ntfs-browser/file-record.h"
    "../include/ntfs-browser/data/file-record-header.h"
    "attr/attribute-list.h"
    "attr/filename.h"
    "attr/header-non-resident.h"
    "attr/header-resident.h"
    "attr/index-root.h"
    "attr/standard-information.h"
    "attr/volume-information.h"
    "attr-bitmap.h"
    "attr-data.h"
    "attr-file-name.h"
    "attr-index-alloc.h"
    "attr-index-root.h"
    "attr-list.h"
    "attr-non-resident.h"
    "attr-resident.h"
    "attr-std-info.h"
    "attr-vol-info.h"
    "attr-vol-name.h"
    "data/index-block.h"
    "data/index-entry.h"
    "data/ntfs-bpb.h"
    "data/run-entry.h"
    "efs/efs-context.h"
    "efs/efs-stream.h"
    "efs/fek.h"
    "efs/sector-cipher.h"
    "flag/filename-namespace.h"
    "flag/filename.h"
    "flag/index-entry.h"
    "flag/std-info-permission.h"
    "index-block.h"
    "internal-export.h"
    "lznt1/decompress.h"
    "ntfs-common.h"
    "utf.h")

set(${PROJECT_NAME}_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")

add_library(${PROJECT_NAME} ${${PROJECT_NAME}_SRC} ${${PROJECT_NAME}_H}
                            ${${PROJECT_NAME}_CMAKE})

include(GenerateExportHeader)
# Per-symbol export/import, replacing a blanket
# CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS: only include/ntfs-browser/*.h types that
# cross the DLL boundary carry NTFS_BROWSER_EXPORT. A static build (the default)
# gets an empty macro, so this header is safe to include either way.
generate_export_header(
  ${PROJECT_NAME} BASE_NAME NTFS_BROWSER EXPORT_FILE_NAME
  "${CMAKE_CURRENT_BINARY_DIR}/include/ntfs-browser/export.h")

target_link_libraries(${PROJECT_NAME} PUBLIC Microsoft.GSL::GSL)
# Private: spdlog is an implementation detail, absent from the public API.
target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog)
# Private too: frozen's maps back internal static lookups only, never a public header.
target_link_libraries(${PROJECT_NAME} PRIVATE frozen::frozen)
# Private too: Crypto++ and the Windows crypto libraries stay out of the API.
# Narrower than the root CMakeLists.txt "EFS master" gate that decides whether
# Crypto++ is configured at all: the shipped library stays Crypto++-free
# whenever it does not itself use that backend, even though the local build tree
# still builds Crypto++ as the unit tests' own reference cipher.
if(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP)
  target_link_libraries(${PROJECT_NAME} PRIVATE cryptopp::cryptopp)
endif()
if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE bcrypt crypt32 ncrypt)
endif()
target_include_directories(
  ${PROJECT_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  # Unlike MSVC, GCC doesn't search transitively-including files' directories.
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(${PROJECT_NAME} PUBLIC "_UNICODE" "UNICODE")
# Feed the three options through as macros to the private .cpp files that test
# them with #ifdef/#if defined(...).
target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE
    $<$<BOOL:${NTFS_BROWSER_ENABLE_DECOMPRESSION}>:NTFS_BROWSER_ENABLE_DECOMPRESSION>
    $<$<BOOL:${NTFS_BROWSER_ENABLE_EFS_CRYPTOPP}>:NTFS_BROWSER_ENABLE_EFS_CRYPTOPP>
    $<$<BOOL:${NTFS_BROWSER_ENABLE_EFS_BCRYPT}>:NTFS_BROWSER_ENABLE_EFS_BCRYPT>)

group_src(${${PROJECT_NAME}_SRC} ${${PROJECT_NAME}_H} ${${PROJECT_NAME}_CMAKE})
