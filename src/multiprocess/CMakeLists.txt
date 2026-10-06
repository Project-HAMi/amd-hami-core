
# NOT USED BY THE AMD BUILD. Leftover from the NVIDIA HAMi-core this project
# was forked from: CMakeLists.txt.hip (the real AMD/HIP build entry point)
# never add_subdirectory()s this directory, references the AMD-native
# hip_multiprocess_memory_limit.c directly instead, and neither
# multiprocess_memory_limit.c nor shrreg_tool.c exist in this tree. Keeping
# this file only as a record of what the NVIDIA layout looked like.
add_library(multiprocess_mod OBJECT multiprocess_memory_limit.c multiprocess_utilization_watcher.c)
target_compile_options(multiprocess_mod PUBLIC ${LIBRARY_COMPILE_FLAGS})
target_link_libraries(multiprocess_mod PUBLIC nvidia-ml)

add_executable(shrreg-tool shrreg_tool.c)
target_link_libraries(shrreg-tool multiprocess_mod -lpthread -lcuda)

