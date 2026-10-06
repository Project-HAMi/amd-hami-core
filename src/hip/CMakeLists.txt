# NOT USED BY THE AMD BUILD. hook.c and memory.c predate the switch to the
# LD_AUDIT approach in libamvgpu_audit.c; CMakeLists.txt.hip (the real
# AMD/HIP build entry point) never add_subdirectory()s this directory and
# lists src/hip/libamvgpu_audit.c directly instead. Keeping this file only
# as a record, same as src/multiprocess/CMakeLists.txt.
set(HIP_SRC
    hook.c
    memory.c
)

add_library(hip_obj OBJECT ${HIP_SRC})
target_include_directories(hip_obj PRIVATE
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_SOURCE_DIR}/src/multiprocess
    ${ROCM_HOME}/include
    ${ROCM_HOME}/include/hip
)
target_compile_options(hip_obj PRIVATE -fPIC -D_GNU_SOURCE -Wall -Wextra)
