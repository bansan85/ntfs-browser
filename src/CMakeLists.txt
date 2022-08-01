add_library(
  ${PROJECT_NAME}
  attr-base.cpp
  attr-index-alloc.cpp
  attr-index-root.cpp
  attr-non-resident.cpp
  attr-resident.cpp
  attr-std-info.cpp
  file-name.cpp
  file-record.cpp
  ntfs-volume.cpp)

target_include_directories(
  ${PROJECT_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
