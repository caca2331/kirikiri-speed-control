cmake_minimum_required(VERSION 3.20)

# This script configures + builds both x64 and x86 variants and stages
# the deliverables into dist/x64 and dist/x86.

set(SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
file(REAL_PATH "${SOURCE_DIR}" SOURCE_DIR)
set(BUILD_X64 "${SOURCE_DIR}/build.x64")
set(BUILD_X86 "${SOURCE_DIR}/build.x86")
set(DIST_DIR "${SOURCE_DIR}/dist")
set(CONFIG "Release")

function(configure_and_build name build_dir arch_triplet arch_flag)
    file(MAKE_DIRECTORY "${build_dir}")
    message(STATUS "[${name}] Configuring (${arch_flag})")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${build_dir}" -A "${arch_flag}"
                -DBUILD_GUI=ON -DBUILD_TESTS=OFF
                -DVCPKG_TARGET_TRIPLET=${arch_triplet}
        RESULT_VARIABLE cfg_r
    )
    if(NOT cfg_r EQUAL 0)
        message(FATAL_ERROR "[${name}] CMake configure failed with code ${cfg_r}")
    endif()

    message(STATUS "[${name}] Building (${CONFIG})")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${build_dir}" --config "${CONFIG}"
        RESULT_VARIABLE bld_r
    )
    if(NOT bld_r EQUAL 0)
        message(FATAL_ERROR "[${name}] Build failed with code ${bld_r}")
    endif()
endfunction()

function(find_soundtouch triplet out_path)
    # Prefer bundled binaries in externals/soundtouch.
    set(arch_dir "x64")
    if(triplet STREQUAL "x86-windows")
        set(arch_dir "x86")
    endif()
    set(ext_candidate "${SOURCE_DIR}/externals/soundtouch/bin/${arch_dir}/SoundTouch.dll")
    if(EXISTS "${ext_candidate}")
        set(${out_path} "${ext_candidate}" PARENT_SCOPE)
        return()
    endif()

    set(root "$ENV{VCPKG_ROOT}")
    if(NOT root)
        set(root "C:/vcpkg")
    endif()
    set(candidate "${root}/installed/${triplet}/bin/SoundTouch.dll")
    if(EXISTS "${candidate}")
        set(${out_path} "${candidate}" PARENT_SCOPE)
    else()
        set(${out_path} "" PARENT_SCOPE)
    endif()
endfunction()

function(stage name build_dir dist_dir triplet)
    set(bin_dir "${build_dir}/${CONFIG}")
    file(MAKE_DIRECTORY "${dist_dir}")
    set(files
        KrkrSpeedController.exe
        krkr_injector.exe
        krkr_speed_hook.dll
        SoundTouch.dll
    )
    foreach(f IN LISTS files)
        set(src "${bin_dir}/${f}")
        if(NOT EXISTS "${src}")
            if(f STREQUAL "SoundTouch.dll")
                find_soundtouch("${triplet}" vcpkg_snd)
                if(vcpkg_snd)
                    set(src "${vcpkg_snd}")
                    message(STATUS "[${name}] Using vcpkg SoundTouch from ${vcpkg_snd}")
                else()
                    message(WARNING "[${name}] Missing ${src}; skipping copy")
                    continue()
                endif()
            else()
                message(WARNING "[${name}] Missing ${src}; skipping copy")
                continue()
            endif()
        endif()
        set(dst "${dist_dir}/${f}")
        if(EXISTS "${dst}")
            file(REMOVE "${dst}")
        endif()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${src}" "${dst}"
            RESULT_VARIABLE copy_r
        )
        if(NOT copy_r EQUAL 0)
            message(WARNING "[${name}] Failed to copy ${src} to ${dst} (result ${copy_r}); file may be locked or in use")
        endif()
    endforeach()
endfunction()

configure_and_build("x64" "${BUILD_X64}" "x64-windows" "x64")
configure_and_build("x86" "${BUILD_X86}" "x86-windows" "Win32")

stage("x64" "${BUILD_X64}" "${DIST_DIR}/x64" "x64-windows")
stage("x86" "${BUILD_X86}" "${DIST_DIR}/x86" "x86-windows")

message(STATUS "Dual-arch staging complete: ${DIST_DIR}/x64 and ${DIST_DIR}/x86")
