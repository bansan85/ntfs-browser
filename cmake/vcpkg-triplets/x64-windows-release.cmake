include("${VCPKG_ROOT_DIR}/triplets/x64-windows-release.cmake")

if(PORT STREQUAL "spdlog")
  set(VCPKG_CMAKE_CONFIGURE_OPTIONS "-DSPDLOG_WCHAR_FILENAMES=ON")
endif()
