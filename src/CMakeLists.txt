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
    "efs/efs-context.cpp"
    "efs/efs-stream.cpp"
    "efs/efs.cpp"
    "efs/cipher-cryptopp.cpp"
    "efs/fek.cpp"
    "file-record.cpp"
    "filename.cpp"
    "file-reader.cpp"
    "index-block.cpp"
    "index-entry.cpp"
    "log.cpp"
    "lznt1/decompress.cpp"
    "ntfs-volume.cpp"
    "utf.cpp")

if(WIN32)
  list(APPEND ${PROJECT_NAME}_SRC "win32-disk-reader.cpp"
       "efs/cipher-bcrypt.cpp" "efs/key-provider-win.cpp")
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
    "lznt1/decompress.h"
    "ntfs-common.h"
    "utf.h")

set(${PROJECT_NAME}_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")

add_library(${PROJECT_NAME} ${${PROJECT_NAME}_SRC} ${${PROJECT_NAME}_H}
                            ${${PROJECT_NAME}_CMAKE})
target_link_libraries(${PROJECT_NAME} PUBLIC Microsoft.GSL::GSL)
# Private: spdlog is an implementation detail, absent from the public API.
target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog)
# Private too: Crypto++ and the Windows crypto libraries stay out of the API.
target_link_libraries(${PROJECT_NAME} PRIVATE cryptopp::cryptopp)
if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE bcrypt crypt32 ncrypt)
endif()
target_include_directories(
  ${PROJECT_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  # Unlike MSVC, GCC doesn't search transitively-including files' directories.
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(${PROJECT_NAME} PUBLIC "_UNICODE" "UNICODE")

group_src(${${PROJECT_NAME}_SRC} ${${PROJECT_NAME}_H} ${${PROJECT_NAME}_CMAKE})
