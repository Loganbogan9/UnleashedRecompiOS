find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(APPLE)
    enable_language(OBJC OBJCXX)
endif()
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

add_executable(touch_gamepad_tests touch_gamepad_tests.cpp)
target_include_directories(touch_gamepad_tests PRIVATE "${CMAKE_SOURCE_DIR}/UnleashedRecomp"
    "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/XenonUtils")
target_compile_options(touch_gamepad_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME ios_touch_gamepad COMMAND touch_gamepad_tests)

set(KERNEL_SYNC_TEST_FLAGS)
if(UNLEASHED_RECOMP_TEST_SANITIZERS)
    list(APPEND KERNEL_SYNC_TEST_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
endif()
add_test(NAME kernel_synchronization_release
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_kernel_synchronization.py"
        "${CMAKE_SOURCE_DIR}" "${CMAKE_CXX_COMPILER}" ${KERNEL_SYNC_TEST_FLAGS})
set_tests_properties(kernel_synchronization_release PROPERTIES TIMEOUT 60)
add_test(NAME metal_drawable_presentation_release
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_metal_presentation.py"
        "${CMAKE_SOURCE_DIR}" "${CMAKE_CXX_COMPILER}" ${KERNEL_SYNC_TEST_FLAGS})
set_tests_properties(metal_drawable_presentation_release PROPERTIES TIMEOUT 60)
add_test(NAME ios_execution_lifecycle_release
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_ios_lifecycle.py"
        "${CMAKE_SOURCE_DIR}" "${CMAKE_CXX_COMPILER}" ${KERNEL_SYNC_TEST_FLAGS})
set_tests_properties(ios_execution_lifecycle_release PROPERTIES TIMEOUT 60)

# Exercise the production input driver against SDL's virtual controller backend.
# Disable unrelated host subsystems so this also runs in headless Linux CI.
function(add_controller_input_tests)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(SDL_SHARED OFF)
    set(SDL_STATIC ON)
    set(SDL_TEST OFF)
    set(SDL_TESTS OFF)
    set(SDL_AUDIO OFF)
    set(SDL_VIDEO OFF)
    set(SDL_RENDER OFF)
    set(SDL_HAPTIC OFF)
    # This SDL version makes its virtual driver depend on HIDAPI being enabled.
    set(SDL_HIDAPI ON)
    set(SDL_HIDAPI_JOYSTICK OFF)
    set(SDL_VIRTUAL_JOYSTICK ON)
    set(SDL_SENSOR OFF)
    set(SDL_POWER OFF)
    set(SDL_CCACHE OFF)
    set(SDL2_DISABLE_INSTALL ON)
    add_subdirectory("${CMAKE_SOURCE_DIR}/thirdparty/SDL" "${CMAKE_CURRENT_BINARY_DIR}/input_sdl" EXCLUDE_FROM_ALL)
    find_package(Threads REQUIRED)
    add_executable(sdl_controller_input portable/sdl_controller_input.cpp
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp/hid/driver/sdl_hid.cpp"
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp/hid/hid.cpp")
    target_include_directories(sdl_controller_input PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/portable/input_stubs"
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp" "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/XenonUtils")
    target_compile_options(sdl_controller_input PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -UNDEBUG
        -include "${CMAKE_CURRENT_SOURCE_DIR}/portable/input_stubs/stdafx.h")
    target_compile_definitions(sdl_controller_input PRIVATE UNLEASHED_RECOMP_IOS)
    target_link_libraries(sdl_controller_input PRIVATE SDL2::SDL2-static Threads::Threads)
    if(APPLE)
        target_sources(sdl_controller_input PRIVATE portable/ios_event_loop.mm
            "${CMAKE_SOURCE_DIR}/UnleashedRecomp/ui/ios_event_loop.mm")
        target_compile_options(sdl_controller_input PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
        target_compile_definitions(sdl_controller_input PRIVATE INPUT_TEST_IOS_LOOP UNLEASHED_RECOMP_METAL)
        target_link_libraries(sdl_controller_input PRIVATE "-framework Foundation")
    endif()
    add_test(NAME sdl_controller_menu_input COMMAND sdl_controller_input)
    set_tests_properties(sdl_controller_menu_input PROPERTIES TIMEOUT 30)
endfunction()
add_controller_input_tests()

if(UNIX)
    # Rename only the mapping call in this test's production utility, allowing
    # deterministic mapping failures on both Linux and macOS.
    add_library(installer_test_mapped_file OBJECT
        "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/XenonUtils/memory_mapped_file.cpp")
    target_compile_definitions(installer_test_mapped_file PRIVATE mmap=InstallerTestMmap)
    add_executable(installer_file_systems
        portable/installer_file_systems.cpp
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp/install/iso_file_system.cpp"
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp/install/xcontent_file_system.cpp")
    target_include_directories(installer_file_systems PRIVATE "${CMAKE_SOURCE_DIR}/UnleashedRecomp"
        "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/XenonUtils")
    target_compile_options(installer_file_systems PRIVATE -fno-strict-aliasing -Wall -Wextra -UNDEBUG)
    target_link_libraries(installer_file_systems PRIVATE installer_test_mapped_file Threads::Threads)
    add_test(NAME installer_file_system_bounds COMMAND installer_file_systems)

    add_executable(installer_ios_file_systems
        portable/installer_file_systems.cpp
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp/install/iso_file_system.cpp"
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp/install/xcontent_file_system.cpp")
    target_include_directories(installer_ios_file_systems PRIVATE "${CMAKE_SOURCE_DIR}/UnleashedRecomp"
        "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/XenonUtils")
    target_compile_definitions(installer_ios_file_systems PRIVATE UNLEASHED_RECOMP_IOS)
    target_compile_options(installer_ios_file_systems PRIVATE -fno-strict-aliasing -Wall -Wextra -UNDEBUG)
    target_link_libraries(installer_ios_file_systems PRIVATE installer_test_mapped_file Threads::Threads)
    add_test(NAME installer_ios_bounded_sources COMMAND installer_ios_file_systems)

    add_executable(installer_streaming
        portable/installer_streaming.cpp
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp/install/installer.cpp"
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp/install/iso_file_system.cpp"
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp/install/xcontent_file_system.cpp"
        "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/XenonUtils/memory_mapped_file.cpp")
    target_include_directories(installer_streaming PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/portable/installer_stubs"
        "${CMAKE_SOURCE_DIR}/UnleashedRecomp" "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/XenonUtils"
        "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/thirdparty/xxHash"
        "${CMAKE_SOURCE_DIR}/tools/XenosRecomp/thirdparty/fmt/include")
    target_compile_definitions(installer_streaming PRIVATE FMT_HEADER_ONLY)
    target_compile_options(installer_streaming PRIVATE -fno-strict-aliasing -Wall -Wextra -UNDEBUG
        -include "${CMAKE_CURRENT_SOURCE_DIR}/portable/installer_stubs/stdafx.h")
    target_link_libraries(installer_streaming PRIVATE Threads::Threads)
    add_test(NAME installer_streaming_and_rollback COMMAND installer_streaming)
endif()

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
