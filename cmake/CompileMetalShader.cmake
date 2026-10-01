include_guard(GLOBAL)
set(UNLEASHED_RECOMP_EMBED_BINARY_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/EmbedBinary.cmake")

function(unleashed_compile_metal_shader target source name)
    # Keep platform-specific libraries and their C embeds in the build tree.
    # A pre-generated macOS embed must never override a new iPhoneOS library.
    set(output "${CMAKE_CURRENT_BINARY_DIR}/generated/gpu/shader/msl/${name}.metal")
    get_filename_component(source_dir "${source}" DIRECTORY)
    file(GLOB includes CONFIGURE_DEPENDS "${source_dir}/*.metali")
    set(target_flags)
    if(UNLEASHED_RECOMP_METAL_TARGET)
        list(APPEND target_flags -target "${UNLEASHED_RECOMP_METAL_TARGET}")
    endif()
    add_custom_command(
        OUTPUT "${output}.ir"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated/gpu/shader/msl"
        COMMAND "${XCRUN_TOOL}" -sdk "${UNLEASHED_RECOMP_APPLE_SDK}" metal
            ${target_flags} -o "${output}.ir" -c "${source}" -D__air__ -frecord-sources -gline-tables-only
        DEPENDS "${source}" ${includes} ${ARGN}
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${output}.metallib"
        COMMAND "${XCRUN_TOOL}" -sdk "${UNLEASHED_RECOMP_APPLE_SDK}" metallib
            -o "${output}.metallib" "${output}.ir"
        DEPENDS "${output}.ir"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${output}.metallib.c" "${output}.metallib.h"
        COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${output}.metallib" "-DOUTPUT_C=${output}.metallib.c"
            "-DOUTPUT_H=${output}.metallib.h" "-DSYMBOL=g_${name}_air"
            -P "${UNLEASHED_RECOMP_EMBED_BINARY_SCRIPT}"
        DEPENDS "${output}.metallib" "${UNLEASHED_RECOMP_EMBED_BINARY_SCRIPT}"
        VERBATIM
    )
    set_source_files_properties("${output}.metallib.c" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_sources(${target} PRIVATE "${output}.metallib.c" "${output}.metallib.h")
    target_include_directories(${target} BEFORE PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
endfunction()
