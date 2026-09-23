# 3.25: the Crypto++ integration below uses block(SCOPE_FOR VARIABLES).
cmake_minimum_required(VERSION 3.25)

project(NtfsBrowser)

include("cmake/vs.cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)

if(VCPKG_TOOLCHAIN)
  set(NTFS_BROWSER_USE_INSTALLED_DEFAULT ON)
else()
  set(NTFS_BROWSER_USE_INSTALLED_DEFAULT OFF)
endif()

option(NTFS_BROWSER_USE_INSTALLED_GSL
       "Link an installed Microsoft GSL instead of the 3rdparty submodule"
       ${NTFS_BROWSER_USE_INSTALLED_DEFAULT})
if(NTFS_BROWSER_USE_INSTALLED_GSL)
  find_package(Microsoft.GSL CONFIG REQUIRED)
else()
  add_subdirectory("3rdparty/gsl")
endif()

option(NTFS_BROWSER_USE_INSTALLED_CATCH2
       "Link an installed Catch2 instead of the 3rdparty submodule"
       ${NTFS_BROWSER_USE_INSTALLED_DEFAULT})
if(NTFS_BROWSER_USE_INSTALLED_CATCH2)
  find_package(Catch2 3 CONFIG REQUIRED)
else()
  add_subdirectory("3rdparty/Catch2")
endif()

option(NTFS_BROWSER_USE_INSTALLED_SPDLOG
       "Link an installed spdlog instead of the 3rdparty submodule"
       ${NTFS_BROWSER_USE_INSTALLED_DEFAULT})
if(NTFS_BROWSER_USE_INSTALLED_SPDLOG)
  find_package(spdlog CONFIG REQUIRED)
else()
  set(SPDLOG_BUILD_EXAMPLE
      OFF
      CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_TESTS
      OFF
      CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_BENCH
      OFF
      CACHE BOOL "" FORCE)
  # Vendored headers are not this project's code to lint.
  set(SPDLOG_SYSTEM_INCLUDES
      ON
      CACHE BOOL "" FORCE)
  set(SPDLOG_WCHAR_FILENAMES
      ON
      CACHE BOOL "" FORCE)
  add_subdirectory("3rdparty/spdlog")
endif()

# Crypto++ is the default EFS cipher backend. Defaults the same way as the other
# NTFS_BROWSER_USE_INSTALLED_* options above: ON under vcpkg, OFF otherwise
# (build the 3rdparty submodule).
option(NTFS_BROWSER_USE_INSTALLED_CRYPTOPP
       "Link an installed Crypto++ instead of the 3rdparty submodule"
       ${NTFS_BROWSER_USE_INSTALLED_DEFAULT})

# LZNT1 decompression and the two EFS decryption cipher backends are each
# optional: turning one off drops its source files, dependency links and call
# sites entirely, so a consumer who never touches compressed/encrypted files
# does not have to pull in Crypto++ (or, on Windows, BCrypt/NCrypt/Crypt32).
option(NTFS_BROWSER_ENABLE_DECOMPRESSION "Compile in LZNT1 decompression" ON)
option(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
       "Compile in the Crypto++ EFS decryption backend" ON)
option(NTFS_BROWSER_ENABLE_EFS_BCRYPT
       "Compile in the Windows BCrypt EFS decryption backend (Windows only)" ON)

# Whether EFS decryption is compiled in at all, in either backend. Mirrors the
# gate src/CMakeLists.txt uses to decide whether efs-context.cpp et al. are
# compiled, and NTFSLibTests/unit-tests/CMakeLists.txt uses to decide whether
# efs-tests.cpp et al. are. NTFS_BROWSER_ENABLE_EFS_BCRYPT carries no platform
# scoping of its own (it defaults ON everywhere), so WIN32 must gate it here.
set(NTFS_BROWSER_EFS_MASTER OFF)
if(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP OR (WIN32 AND NTFS_BROWSER_ENABLE_EFS_BCRYPT
                                       ))
  set(NTFS_BROWSER_EFS_MASTER ON)
endif()

if(NTFS_BROWSER_EFS_MASTER)
  if(NTFS_BROWSER_USE_INSTALLED_CRYPTOPP)
    find_package(cryptopp CONFIG REQUIRED)
  else()
    # Crypto++ only supports static libraries, whatever BUILD_SHARED_LIBS says.
    # The block keeps that override out of the rest of the project.
    block(SCOPE_FOR VARIABLES)
    set(BUILD_SHARED_LIBS OFF)
    # A static Crypto++ ends up inside a shared NtfsBrowser on Linux.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CRYPTOPP_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/cryptopp"
        CACHE PATH "" FORCE)
    # Its tests, docs and install rules are not this project's.
    set(CRYPTOPP_BUILD_TESTING
        OFF
        CACHE BOOL "" FORCE)
    set(CRYPTOPP_BUILD_DOCUMENTATION
        OFF
        CACHE BOOL "" FORCE)
    set(CRYPTOPP_INSTALL
        OFF
        CACHE BOOL "" FORCE)
    add_subdirectory("3rdparty/cryptopp-cmake")
    if(MSVC)
      # Crypto++ 8.9 does not build with the newest MSVC STL as it is.
      target_compile_options(
        cryptopp
        PRIVATE
          "$<$<COMPILE_LANGUAGE:CXX>:/FI${CMAKE_CURRENT_SOURCE_DIR}/cmake/cryptopp-stdext-compat.h>"
      )
    endif()
    # The Visual Studio generators do not create the object directory of a MASM
    # source that sits outside the build tree, which Crypto++'s does. Its path
    # is the source's own one, with each ".." spelled "__".
    if(CMAKE_GENERATOR MATCHES "Visual Studio")
      foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
        file(
          MAKE_DIRECTORY
          "${cryptopp-cmake_BINARY_DIR}/cryptopp/cryptopp.dir/${config}/__/__/cryptopp"
        )
      endforeach()
    endif()
    endblock()
  endif()
endif()

add_subdirectory("src")

enable_testing()
add_subdirectory("NTFSLibTests")
