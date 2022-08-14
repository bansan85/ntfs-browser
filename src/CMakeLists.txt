set(${PROJECT_NAME}_SRC
    data/file-record-header.cpp
    attr-base.cpp
    attr-bitmap.cpp
    attr-file-name.cpp
    attr-index-alloc.cpp
    attr-index-root.cpp
    attr-list.cpp
    attr-non-resident.cpp
    attr-resident.cpp
    attr-std-info.cpp
    attr-vol-info.cpp
    attr-vol-name.cpp
    file-record.cpp
    filename.cpp
    index-block.cpp
    index-entry.cpp
    ntfs-volume.cpp)

set(${PROJECT_NAME}_H
    attr/attribute-list.h
    attr/filename.h
    attr/header-non-resident.h
    attr/header-resident.h
    attr/index-root.h
    attr/standard-information.h
    attr/volume-information.h
    attr-bitmap.h
    attr-data.h
    attr-file-name.h
    attr-index-alloc.h
    attr-index-root.h
    attr-list.h
    attr-non-resident.h
    attr-resident.h
    attr-std-info.h
    attr-vol-info.h
    attr-vol-name.h
    data/file-record-header.h
    data/index-block.h
    data/index-entry.h
    data/ntfs-bpb.h
    data/run-entry.h
    flag/file-record.h
    flag/filename-namespace.h
    flag/filename.h
    flag/index-entry.h
    flag/std-info-permission.h
    index-block.h
    ntfs-common.h)

set(${PROJECT_NAME}_CMAKE ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt)

add_library(${PROJECT_NAME} ${${PROJECT_NAME}_SRC} ${${PROJECT_NAME}_H}
                            ${${PROJECT_NAME}_CMAKE})
target_link_libraries(${PROJECT_NAME} PUBLIC Microsoft.GSL::GSL)
target_include_directories(
  ${PROJECT_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(${PROJECT_NAME} PUBLIC "_UNICODE" "UNICODE")

group_src(${${PROJECT_NAME}_SRC} ${${PROJECT_NAME}_H} ${${PROJECT_NAME}_CMAKE})
