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
option(BUILD_TESTS "Build DSP smoke tests (desktop only)" OFF)

# --- SoundTouch dependency (always enabled)
set(SOUNDTOUCH_ROOT "${CMAKE_SOURCE_DIR}/externals/soundtouch")
set(_soundtouch_arch_dir "x64")
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(_soundtouch_arch_dir "x86")
endif()

set(_soundtouch_include "${SOUNDTOUCH_ROOT}/include")
set(_soundtouch_lib "${SOUNDTOUCH_ROOT}/lib/${_soundtouch_arch_dir}/SoundTouch.lib")
set(_soundtouch_dll "${SOUNDTOUCH_ROOT}/bin/${_soundtouch_arch_dir}/SoundTouch.dll")
set(_soundtouch_from_externals OFF)

if(EXISTS "${_soundtouch_lib}" AND EXISTS "${_soundtouch_include}")
    add_library(SoundTouch::SoundTouch SHARED IMPORTED)
    set_target_properties(SoundTouch::SoundTouch PROPERTIES
        IMPORTED_IMPLIB "${_soundtouch_lib}"
        IMPORTED_LOCATION "${_soundtouch_dll}"
    )
    target_include_directories(SoundTouch::SoundTouch INTERFACE "${_soundtouch_include}")
    set(_soundtouch_from_externals ON)
    set(_soundtouch_runtime "${_soundtouch_dll}")
else()
    # Fallback to system/vcpkg package if externals are missing.
    find_package(SoundTouch CONFIG REQUIRED)
    get_target_property(_soundtouch_runtime SoundTouch::SoundTouch IMPORTED_LOCATION)
endif()

add_library(krkr_common STATIC
    src/common/DspPipeline.cpp
    src/common/Logging.cpp
    src/common/EnvOverrides.cpp
    src/common/AudioStreamProcessor.cpp
)
target_include_directories(krkr_common PUBLIC src)
target_compile_definitions(krkr_common PUBLIC USE_SOUNDTOUCH)
# Add SoundTouch headers explicitly for this target to satisfy MSVC include lookup.
target_include_directories(krkr_common PRIVATE
    $<TARGET_PROPERTY:SoundTouch::SoundTouch,INTERFACE_INCLUDE_DIRECTORIES>
)
target_link_libraries(krkr_common PUBLIC SoundTouch::SoundTouch)

function(copy_soundtouch_runtime target)
    if(DEFINED _soundtouch_runtime AND EXISTS "${_soundtouch_runtime}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_soundtouch_runtime}"
                $<TARGET_FILE_DIR:${target}>
            COMMENT "Copying SoundTouch runtime for ${target}"
        )
    elseif(TARGET SoundTouch::SoundTouch)
        get_target_property(_st_runtime SoundTouch::SoundTouch IMPORTED_LOCATION)
        if(NOT _st_runtime)
            get_target_property(_st_runtime SoundTouch::SoundTouch IMPORTED_LOCATION_RELEASE)
        endif()
        if(NOT _st_runtime)
            get_target_property(_st_runtime SoundTouch::SoundTouch IMPORTED_LOCATION_RELWITHDEBINFO)
        endif()
        if(NOT _st_runtime)
            get_target_property(_st_runtime SoundTouch::SoundTouch IMPORTED_LOCATION_MINSIZEREL)
        endif()
        if(NOT _st_runtime)
            get_target_property(_st_runtime SoundTouch::SoundTouch IMPORTED_LOCATION_DEBUG)
        endif()
        if(_st_runtime AND EXISTS "${_st_runtime}")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_st_runtime}"
                    $<TARGET_FILE_DIR:${target}>
                COMMENT "Copying SoundTouch runtime for ${target}"
            )
        endif()
    endif()
endfunction()

if(WIN32)
    add_library(krkr_hook_core STATIC
        src/hook/XAudio2Hook.cpp
        src/hook/DirectSoundHook.cpp
        src/hook/FMODHook.cpp
        src/hook/WwiseHook.cpp
        src/hook/HookUtils.cpp
        src/hook/ModuleScan.cpp
    )
    target_link_libraries(krkr_hook_core PUBLIC krkr_common dsound xaudio2 dxguid winmm)
    target_include_directories(krkr_hook_core PUBLIC src)

    add_executable(krkr_injector
        src/injector/injector_main.cpp
    )
    target_link_libraries(krkr_injector PRIVATE krkr_common)

    add_library(krkr_speed_hook SHARED
        src/hook/dllmain.cpp
    )
    target_link_libraries(krkr_speed_hook PRIVATE krkr_hook_core uuid)
    copy_soundtouch_runtime(krkr_speed_hook)

    if(BUILD_GUI)
        add_executable(KrkrSpeedController WIN32
            src/gui/main.cpp
            src/gui/ui.cpp
        )
        target_link_libraries(KrkrSpeedController PRIVATE krkr_hook_core comctl32 uuid)
        copy_soundtouch_runtime(KrkrSpeedController)
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

if(_soundtouch_from_externals)
    message(STATUS "SoundTouch: using externals/soundtouch (${_soundtouch_arch_dir})")
else()
    message(STATUS "SoundTouch: found via package manager")
endif()
