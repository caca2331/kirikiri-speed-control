cmake_minimum_required(VERSION 3.20)
project(KrkrSpeedHook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # Static CRT to avoid requiring VC++ redist on target machines.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(BUILD_GUI "Build the optional controller GUI" ON)
option(USE_SOUNDTOUCH "Enable SoundTouch based DSP (requires library)" OFF)
option(BUILD_TESTS "Build DSP smoke tests (desktop only)" OFF)

add_library(krkr_common STATIC
    src/common/DspPipeline.cpp
    src/common/Logging.cpp
)
target_include_directories(krkr_common PUBLIC src)
target_compile_definitions(krkr_common PUBLIC
    $<$<BOOL:${USE_SOUNDTOUCH}>:USE_SOUNDTOUCH>
)
if(USE_SOUNDTOUCH)
    # Hint CMake where the vcpkg-provided SoundTouch package lives.
    if(NOT DEFINED SoundTouch_DIR)
        set(_vcpkg_root "$ENV{VCPKG_ROOT}")
        if(NOT _vcpkg_root AND EXISTS "C:/vcpkg")
            set(_vcpkg_root "C:/vcpkg")
        endif()

        if(_vcpkg_root)
            if(DEFINED VCPKG_TARGET_TRIPLET)
                set(_vcpkg_triplet "${VCPKG_TARGET_TRIPLET}")
            elseif(DEFINED ENV{VCPKG_DEFAULT_TRIPLET})
                set(_vcpkg_triplet "$ENV{VCPKG_DEFAULT_TRIPLET}")
            else()
                set(_vcpkg_triplet "x64-windows")
            endif()
            set(SoundTouch_DIR "${_vcpkg_root}/installed/${_vcpkg_triplet}/share/soundtouch" CACHE PATH "Path to SoundTouch package config")
        endif()
    endif()
    find_package(SoundTouch CONFIG REQUIRED)
    # Add SoundTouch headers explicitly for this target to satisfy MSVC include lookup.
    target_include_directories(krkr_common PRIVATE
        $<TARGET_PROPERTY:SoundTouch::SoundTouch,INTERFACE_INCLUDE_DIRECTORIES>
    )
    target_link_libraries(krkr_common PUBLIC SoundTouch::SoundTouch)

    function(copy_soundtouch_runtime target)
        if(NOT TARGET SoundTouch::SoundTouch)
            return()
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SoundTouch::SoundTouch>
                $<TARGET_FILE_DIR:${target}>
            COMMENT "Copying SoundTouch runtime for ${target}"
        )
    endfunction()
endif()

if(WIN32)
    add_library(krkr_hook_core STATIC
        src/hook/XAudio2Hook.cpp
        src/hook/DirectSoundHook.cpp
        src/hook/HookUtils.cpp
        src/hook/ModuleScan.cpp
    )
    target_link_libraries(krkr_hook_core PUBLIC krkr_common dsound xaudio2 dxguid winmm)
    target_include_directories(krkr_hook_core PUBLIC src)

    add_library(krkr_speed_hook SHARED
        src/hook/dllmain.cpp
    )
    target_link_libraries(krkr_speed_hook PRIVATE krkr_hook_core)
    if(USE_SOUNDTOUCH)
        copy_soundtouch_runtime(krkr_speed_hook)
    endif()

    if(BUILD_GUI)
        add_executable(KrkrSpeedController WIN32
            src/gui/main.cpp
            src/gui/ui.cpp
        )
        target_link_libraries(KrkrSpeedController PRIVATE krkr_hook_core comctl32)
        if(USE_SOUNDTOUCH)
            copy_soundtouch_runtime(KrkrSpeedController)
        endif()
    endif()
else()
    message(STATUS "Non-Windows host detected; building common library and tests only.")
endif()

if(BUILD_TESTS)
    message(STATUS "Tests disabled by default; BUILD_TESTS=OFF")
endif()

if(WIN32)
    add_custom_target(dist_dual_arch
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/cmake/dual_arch.cmake
        COMMENT "Configure+build x86/x64 and stage dist/{x86,x64}"
    )
endif()

message(STATUS "SoundTouch support: ${USE_SOUNDTOUCH}")
