add_library(
  ${PROJECT_NAME}
  attr-base.cpp
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

target_include_directories(
  ${PROJECT_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
