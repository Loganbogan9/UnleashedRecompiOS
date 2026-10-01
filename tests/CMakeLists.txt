find_package(Python3 REQUIRED COMPONENTS Interpreter)
option(UNLEASHED_RECOMP_TEST_SANITIZERS "Enable address and undefined-behavior sanitizers in portable tests" OFF)
if(UNLEASHED_RECOMP_TEST_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "Portable sanitizers require Clang or GCC")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
add_test(NAME metal_build_dependencies
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_metal_build.py"
        "${CMAKE_SOURCE_DIR}" "${CMAKE_COMMAND}" "${CMAKE_C_COMPILER}")
add_test(NAME ios_package_structure
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_ios_package.py" "${CMAKE_SOURCE_DIR}")
add_test(NAME ios_configuration_metadata
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_ios_configuration.py"
        "${CMAKE_SOURCE_DIR}" "${CMAKE_COMMAND}" "${CMAKE_CXX_COMPILER}")
add_test(NAME dependency_patch_overlays
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_dependency_overlay.py"
        "${CMAKE_SOURCE_DIR}" "${CMAKE_COMMAND}" "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME ios_shader_cache_provenance
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_shader_provenance.py"
        "${CMAKE_SOURCE_DIR}" "${CMAKE_COMMAND}")
add_test(NAME movie_shader_layout
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_shader_layout.py"
        "${CMAKE_SOURCE_DIR}" "${CMAKE_CXX_COMPILER}")
add_executable(bc_decoder_tests bc_decoder_tests.cpp)
target_include_directories(bc_decoder_tests PRIVATE "${CMAKE_SOURCE_DIR}/UnleashedRecomp")
target_include_directories(bc_decoder_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/thirdparty/ddspp")
target_compile_options(bc_decoder_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME bc_texture_decoder COMMAND bc_decoder_tests)
set(PLUME_TEST_FLAGS -UNDEBUG)
if(UNLEASHED_RECOMP_TEST_SANITIZERS)
    list(APPEND PLUME_TEST_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
endif()
add_test(NAME plume_metal_push_constants
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_plume_metal.py"
        "${CMAKE_SOURCE_DIR}" "${CMAKE_CXX_COMPILER}" ${PLUME_TEST_FLAGS})
add_test(NAME plume_metal_descriptor_traversal
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_metal_descriptor_traversal.py"
        "${CMAKE_SOURCE_DIR}" "${CMAKE_CXX_COMPILER}" ${PLUME_TEST_FLAGS})
add_executable(xex_load_tests xex_load_tests.cpp)
target_include_directories(xex_load_tests PRIVATE "${CMAKE_SOURCE_DIR}/UnleashedRecomp"
    "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/XenonUtils")
target_compile_options(xex_load_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME xex_load_bounds COMMAND xex_load_tests)
add_executable(bounded_log_tests bounded_log_tests.cpp)
target_include_directories(bounded_log_tests PRIVATE "${CMAKE_SOURCE_DIR}/UnleashedRecomp")
target_compile_options(bounded_log_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME bounded_log_rotation COMMAND bounded_log_tests)
add_executable(launch_request_tests launch_request_tests.cpp)
target_include_directories(launch_request_tests PRIVATE "${CMAKE_SOURCE_DIR}/UnleashedRecomp")
target_compile_options(launch_request_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME ios_next_launch_request COMMAND launch_request_tests)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(test_o1heap STATIC "${CMAKE_SOURCE_DIR}/thirdparty/o1heap/o1heap.c")
    target_include_directories(test_o1heap PUBLIC "${CMAKE_SOURCE_DIR}/thirdparty/o1heap")
    add_executable(runtime_memory_heap
        portable/runtime_memory_heap.cpp
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp/kernel/memory.cpp"
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp/kernel/heap.cpp")
    target_include_directories(runtime_memory_heap PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/portable/runtime_stubs"
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp" "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/XenonUtils")
    target_compile_options(runtime_memory_heap PRIVATE -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter -UNDEBUG)
    target_link_libraries(runtime_memory_heap PRIVATE test_o1heap)
    add_test(NAME runtime_memory_heap COMMAND runtime_memory_heap)
    add_executable(installer_file_systems
        portable/installer_file_systems.cpp
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp/install/iso_file_system.cpp"
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp/install/xcontent_file_system.cpp"
        "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/XenonUtils/memory_mapped_file.cpp")
    target_include_directories(installer_file_systems PRIVATE "${CMAKE_SOURCE_DIR}/UnleashedRecomp"
        "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/XenonUtils")
    target_compile_options(installer_file_systems PRIVATE -fno-strict-aliasing -Wall -Wextra -UNDEBUG)
    target_link_options(installer_file_systems PRIVATE -Wl,--wrap=mmap)
    add_test(NAME installer_file_system_bounds COMMAND installer_file_systems)
    add_executable(guest_file_io
        portable/guest_file_io.cpp
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp/kernel/io/file_system.cpp"
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp/kernel/heap.cpp"
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp/kernel/memory.cpp"
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp/kernel/xdm.cpp")
    target_include_directories(guest_file_io PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/portable/file_io_stubs"
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp" "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/XenonUtils")
    target_compile_options(guest_file_io PRIVATE -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter -UNDEBUG
        -include "${CMAKE_CURRENT_SOURCE_DIR}/portable/file_io_stubs/stdafx.h")
    target_link_libraries(guest_file_io PRIVATE test_o1heap)
    add_test(NAME guest_file_io COMMAND guest_file_io)
    find_package(Threads REQUIRED)
    add_library(test_xxhash STATIC "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/thirdparty/xxHash/xxhash.c")
    target_include_directories(test_xxhash PUBLIC "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/thirdparty/xxHash")
    foreach(thread_variant IN ITEMS pthread ios_stack)
        add_executable(guest_threads_${thread_variant}
            portable/guest_threads.cpp
            "${CMAKE_SOURCE_DIR}/UnleashedRecomp/cpu/guest_thread.cpp"
            "${CMAKE_SOURCE_DIR}/UnleashedRecomp/kernel/heap.cpp"
            "${CMAKE_SOURCE_DIR}/UnleashedRecomp/kernel/memory.cpp"
            "${CMAKE_SOURCE_DIR}/UnleashedRecomp/kernel/xdm.cpp")
        target_include_directories(guest_threads_${thread_variant} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/portable/thread_stubs"
            "${CMAKE_SOURCE_DIR}/UnleashedRecomp" "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/XenonUtils")
        target_compile_options(guest_threads_${thread_variant} PRIVATE -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter -UNDEBUG)
        target_compile_definitions(guest_threads_${thread_variant} PRIVATE USE_PTHREAD=1)
        if(thread_variant STREQUAL "ios_stack")
            target_compile_definitions(guest_threads_${thread_variant} PRIVATE UNLEASHED_RECOMP_IOS=1)
        endif()
        target_link_libraries(guest_threads_${thread_variant} PRIVATE test_o1heap test_xxhash Threads::Threads)
        target_link_options(guest_threads_${thread_variant} PRIVATE -Wl,--wrap=pthread_create
            -Wl,--wrap=pthread_attr_setstacksize -Wl,--wrap=pthread_join)
        add_test(NAME guest_threads_${thread_variant} COMMAND guest_threads_${thread_variant})
    endforeach()
    add_executable(guest_semaphore portable/guest_semaphore.cpp)
    target_include_directories(guest_semaphore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/portable/runtime_stubs"
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp" "${CMAKE_SOURCE_DIR}/thirdparty/o1heap"
        "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/XenonUtils")
    target_compile_options(guest_semaphore PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter -UNDEBUG)
    target_link_libraries(guest_semaphore PRIVATE Threads::Threads)
    add_test(NAME guest_semaphore_polling COMMAND guest_semaphore)
    add_library(guest_thread_std_compile OBJECT "${CMAKE_SOURCE_DIR}/UnleashedRecomp/cpu/guest_thread.cpp")
    target_include_directories(guest_thread_std_compile PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/portable/thread_stubs"
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp" "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/XenonUtils")
    target_compile_options(guest_thread_std_compile PRIVATE -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter -UNDEBUG)
    target_link_libraries(guest_thread_std_compile PRIVATE test_o1heap test_xxhash Threads::Threads)
    # Use the generator's pinned fmt rather than a distro version that may
    # predate fmt::println (e.g. Ubuntu 24.04). No vcpkg or downloads needed.
    set(FMT_DOC OFF CACHE BOOL "" FORCE)
    set(FMT_TEST OFF CACHE BOOL "" FORCE)
    set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/tools/XenosRecomp/thirdparty/fmt" "fmt")
    include("${CMAKE_SOURCE_DIR}/cmake/PrepareDependency.cmake")
    unleashed_prepare_dependency(TEST_XENOS_SOURCE xenos-tests "${CMAKE_SOURCE_DIR}/tools/XenosRecomp"
        PATCHES "${CMAKE_SOURCE_DIR}/patches/dependencies/xenos-air-target.patch" COPY_DIRS XenosRecomp)
    foreach(sdk IN ITEMS macosx iphoneos)
        if(sdk STREQUAL "iphoneos")
            set(air_target air64-apple-ios16.0)
        else()
            set(air_target air64-apple-macos13.0)
        endif()
        add_executable(air_command_${sdk} air_command_generation.cpp "${TEST_XENOS_SOURCE}/XenosRecomp/air_compiler.cpp")
        target_include_directories(air_command_${sdk} PRIVATE "${TEST_XENOS_SOURCE}/XenosRecomp")
        target_compile_definitions(air_command_${sdk} PRIVATE XENOS_RECOMP_AIR_SDK="${sdk}" XENOS_RECOMP_AIR_TARGET="${air_target}")
        target_compile_options(air_command_${sdk} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
        target_link_libraries(air_command_${sdk} PRIVATE fmt::fmt)
        target_link_options(air_command_${sdk} PRIVATE -Wl,--wrap=posix_spawn -Wl,--wrap=waitpid -Wl,--wrap=write)
        add_test(NAME air_host_commands_${sdk} COMMAND air_command_${sdk})
    endforeach()
endif()
