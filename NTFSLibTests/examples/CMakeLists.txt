add_subdirectory("ntfsdir")
add_subdirectory("ntfsmftlist")

if(WIN32)
  add_subdirectory("ntfsattr")
  add_subdirectory("ntfsdump")
  add_subdirectory("ntfsundel")
endif()
