# Runs NtfsFuzzGen twice into two empty folders and fails when any file differs.
#
# usage: cmake -DGENERATOR=<NtfsFuzzGen> -DRESOURCES=<folder of the extracted
#        archives> -DWORK=<scratch folder> -P check-determinism.cmake
file(REMOVE_RECURSE "${WORK}")

foreach(run IN ITEMS first second)
  execute_process(
    COMMAND "${GENERATOR}" --out "${WORK}/${run}" --resources "${RESOURCES}"
    RESULT_VARIABLE result
    OUTPUT_QUIET
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "NtfsFuzzGen failed in the ${run} run (${result})")
  endif()
endforeach()

file(GLOB_RECURSE first_files RELATIVE "${WORK}/first" "${WORK}/first/*")
file(GLOB_RECURSE second_files RELATIVE "${WORK}/second" "${WORK}/second/*")
if(NOT first_files STREQUAL second_files)
  message(FATAL_ERROR "the two runs wrote different sets of files")
endif()
list(LENGTH first_files count)
if(count EQUAL 0)
  message(FATAL_ERROR "NtfsFuzzGen wrote no file")
endif()

set(different)
foreach(file IN LISTS first_files)
  file(SHA256 "${WORK}/first/${file}" first_hash)
  file(SHA256 "${WORK}/second/${file}" second_hash)
  if(NOT first_hash STREQUAL second_hash)
    list(APPEND different "${file}")
  endif()
endforeach()
if(different)
  message(FATAL_ERROR "files that differ between the runs: ${different}")
endif()

file(REMOVE_RECURSE "${WORK}")
message(STATUS "${count} files, identical in both runs")
