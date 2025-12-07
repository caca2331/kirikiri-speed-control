cmake_minimum_required(VERSION 3.20)
project(KrkrSpeedHook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_GUI "Build the optional controller GUI" ON)
option(USE_SOUNDTOUCH "Enable SoundTouch based DSP (requires library)" OFF)
option(BUILD_TESTS "Build DSP smoke tests (desktop only)" ON)

add_library(krkr_common STATIC
    src/common/DspPipeline.cpp
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
endif()

if(WIN32)
    add_library(krkr_speed_hook SHARED
        src/hook/dllmain.cpp
        src/hook/XAudio2Hook.cpp
        src/hook/DirectSoundHook.cpp
    )
    target_link_libraries(krkr_speed_hook PRIVATE krkr_common)

    if(BUILD_GUI)
        add_executable(KrkrSpeedController
            src/gui/main.cpp
        )
        target_link_libraries(KrkrSpeedController PRIVATE krkr_common)
    endif()
else()
    message(STATUS "Non-Windows host detected; building common library and tests only.")
endif()

if(BUILD_TESTS)
    enable_testing()
    add_executable(dsp_smoke
        tests/dsp_smoke.cpp
    )
    target_link_libraries(dsp_smoke PRIVATE krkr_common)
    add_test(NAME dsp_smoke COMMAND dsp_smoke)
endif()

message(STATUS "SoundTouch support: ${USE_SOUNDTOUCH}")
