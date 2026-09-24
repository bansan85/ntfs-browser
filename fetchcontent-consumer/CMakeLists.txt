cmake_minimum_required(VERSION 3.25)

project(NtfsBrowserFetchContentSmokeTest CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include(FetchContent)

# Every NTFS_BROWSER_* toggle ON, plus a shared build: this smoke test exists to
# catch a combination that only builds under the library's own defaults, not
# under a consumer's explicit choices.
set(BUILD_SHARED_LIBS
    ON
    CACHE BOOL "" FORCE)
set(NTFS_BROWSER_ENABLE_DECOMPRESSION
    ON
    CACHE BOOL "" FORCE)
set(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
    ON
    CACHE BOOL "" FORCE)
set(NTFS_BROWSER_ENABLE_EFS_BCRYPT
    ON
    CACHE BOOL "" FORCE)

# Points FetchContent at an already-checked-out working tree (eg. this
# repository's own CI, which already has one) instead of cloning over the
# network. Left unset, it falls back to a real GIT_REPOSITORY fetch, which is
# the FetchContent path an external consumer actually takes.
if(NTFS_BROWSER_FETCHCONTENT_SOURCE_DIR)
  FetchContent_Declare(ntfs-browser SOURCE_DIR
                                    "${NTFS_BROWSER_FETCHCONTENT_SOURCE_DIR}")
else()
  FetchContent_Declare(
    ntfs-browser
    GIT_REPOSITORY https://github.com/bansai85/ntfs-browser.git
    GIT_TAG main
    GIT_SUBMODULES_RECURSE ON)
endif()
FetchContent_MakeAvailable(ntfs-browser)

add_executable(fetchcontent-smoke-test "main.cpp")
target_link_libraries(fetchcontent-smoke-test PRIVATE NtfsBrowser)
# Tells main.cpp whether an EFS backend is actually linked in, mirroring the
# NTFS_BROWSER_ENABLE_EFS_CRYPTOPP/_BCRYPT choice above: GetCipherBackend() has
# no definition to call when neither is on.
target_compile_definitions(
  fetchcontent-smoke-test
  PRIVATE
    $<$<OR:$<BOOL:${NTFS_BROWSER_ENABLE_EFS_CRYPTOPP}>,$<BOOL:${NTFS_BROWSER_ENABLE_EFS_BCRYPT}>>:FETCHCONTENT_SMOKE_TEST_EFS>
)
if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET fetchcontent-smoke-test
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:fetchcontent-smoke-test>
      $<TARGET_FILE_DIR:fetchcontent-smoke-test>
    COMMAND_EXPAND_LISTS)
endif()
