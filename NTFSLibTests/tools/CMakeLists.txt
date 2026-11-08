add_subdirectory("benchmark")
add_subdirectory("device-access")
add_subdirectory("dir-walk")
add_subdirectory("disk-readers")

if(NTFS_BROWSER_EFS_MASTER)
  add_subdirectory("efs-support")
endif()

add_subdirectory("fake-image")
add_subdirectory("ntfscompare")
