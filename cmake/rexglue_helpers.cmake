#==========================================================
# rexglue_helpers.cmake
#
# Three helpers, each with a single responsibility:
#   rexglue_apply_target_settings(<target>)        - common compile/platform flags
#   rexglue_configure_target(<target>)             - host application
#   rexglue_configure_module_target(<target> ...)  - guest DLL module
#==========================================================

#==========================================================
# rexglue_apply_target_settings(<target>) - Common flags
#
# Applied to both host apps and guest DLL modules. Does NOT
# whole-archive any library: rexruntime is SHARED, so all
# kernel-hook __imp__* symbols are in its export table by
# definition.
#==========================================================
function(rexglue_apply_target_settings target_name)
    # Linux platform settings
    if(UNIX AND NOT APPLE)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
        target_include_directories(${target_name} PRIVATE ${GTK3_INCLUDE_DIRS})
        target_link_libraries(${target_name} PRIVATE ${GTK3_LIBRARIES})
        # Large executable support
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            target_link_options(${target_name} PRIVATE -Wl,--no-relax)
            target_compile_options(${target_name} PRIVATE -mcmodel=large)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
            target_compile_options(${target_name} PRIVATE -march=armv8-a)
        endif()
    endif()

    if(NOT MSVC)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            target_compile_options(${target_name} PRIVATE -msse4.1)
        endif()
        # ARM64 NEON is enabled via -march=armv8-a above
    endif()

    # Copy runtime DLLs next to the target on Windows
    if(WIN32)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:${target_name}>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:${target_name}>;$<TARGET_FILE_DIR:${target_name}>>"
            COMMAND_EXPAND_LISTS
        )
        # FidelityFX is linked PRIVATE by rexui (to avoid propagating DLL
        # requirements to tool-mode targets), so copy its DLLs explicitly.
        if(TARGET amd_fidelityfx_vk)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:amd_fidelityfx_vk>
                    $<TARGET_FILE_DIR:${target_name}>
            )
        endif()
        if(TARGET amd_fidelityfx_dx12)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:amd_fidelityfx_dx12>
                    $<TARGET_FILE_DIR:${target_name}>
            )
        endif()
    endif()
endfunction()

#==========================================================
# rexglue_configure_target(<target>) - Host application
#
# Adds:
#   - Platform entry point source (windowed_app_main_*.cpp)
#   - ReXApp base class source (rex_app.cpp)
#   - $ORIGIN RPATH on UNIX so the host finds librexruntime.so next to itself
#==========================================================
function(rexglue_configure_target target_name)
    # Platform entry point
    if(WIN32)
        target_sources(${target_name} PRIVATE
            ${REXGLUE_SHARE_DIR}/windowed_app_main_win.cpp)
    else()
        target_sources(${target_name} PRIVATE
            ${REXGLUE_SHARE_DIR}/windowed_app_main_posix.cpp)
    endif()

    # ReXApp base class
    target_sources(${target_name} PRIVATE
        ${REXGLUE_SHARE_DIR}/rex_app.cpp)

    # Build config for version stamp
    target_compile_definitions(${target_name} PRIVATE
        REXGLUE_BUILD_CONFIG="$<CONFIG>")

    # On UNIX: RPATH $ORIGIN so the host finds librexruntime.so co-located
    if(UNIX AND NOT APPLE)
        set_target_properties(${target_name} PROPERTIES
            INSTALL_RPATH "$ORIGIN"
            BUILD_WITH_INSTALL_RPATH ON
        )
    endif()

    rexglue_apply_target_settings(${target_name})
endfunction()

#==========================================================
# rexglue_configure_module_target(<target> [HOST <host_target>])
#   - Guest DLL module
#
# Adds:
#   - Output directories matched to the host app's, so the guest DLL is
#     loadable when the host calls LoadUserModule. Falls back to
#     CMAKE_RUNTIME_OUTPUT_DIRECTORY if HOST is omitted.
#   - $ORIGIN RPATH on UNIX so the dlopen'd guest DLL finds librexruntime.so
#     in the same directory as the host EXE.
#
# Intentionally does NOT add app entry sources (WinMain/main) or rex_app.cpp,
# because guest DLLs are loaded by the host app rather than launched as
# standalone executables. Linking against rexruntime is wired by the codegen-
# emitted dll_targets.cmake, not here.
#==========================================================
function(rexglue_configure_module_target target_name)
    cmake_parse_arguments(ARG "" "HOST" "" ${ARGN})

    if(ARG_HOST)
        set_target_properties(${target_name} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:${ARG_HOST}>
            RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:${ARG_HOST}>
            ARCHIVE_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:${ARG_HOST}>
        )
    elseif(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set_target_properties(${target_name} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
            ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        )
    endif()

    # On UNIX: RPATH $ORIGIN so the guest DLL finds librexruntime.so co-located
    if(UNIX AND NOT APPLE)
        set_target_properties(${target_name} PROPERTIES
            INSTALL_RPATH "$ORIGIN"
            BUILD_WITH_INSTALL_RPATH ON
        )
    endif()

    rexglue_apply_target_settings(${target_name})
endfunction()
