cmake_minimum_required(VERSION 3.20)
project(Vst3HostNative LANGUAGES CXX)

# ABI_SPEC.md §2: x64 Windows only. There is no x86 build.
if(NOT WIN32)
    message(FATAL_ERROR "Vst3HostNative is Windows-only (ABI_SPEC.md §9).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Vst3HostNative is x64 only. Configure with -A x64.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VST3_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/vst3sdk" CACHE PATH
    "Root of the Steinberg VST3 SDK checkout")
if(NOT EXISTS "${VST3_SDK_DIR}/pluginterfaces/base/funknown.cpp")
    message(FATAL_ERROR
        "VST3 SDK not found at ${VST3_SDK_DIR}.\n"
        "Run: git submodule update --init --recursive")
endif()

# Only the subset of the SDK a host needs — building the SDK's own CMake would
# also pull in VSTGUI, the sample plugins, and the validator, none of which we use.
set(SDK_SOURCES
    ${VST3_SDK_DIR}/pluginterfaces/base/funknown.cpp
    ${VST3_SDK_DIR}/pluginterfaces/base/coreiids.cpp
    ${VST3_SDK_DIR}/pluginterfaces/base/ustring.cpp
    ${VST3_SDK_DIR}/pluginterfaces/base/conststringtable.cpp
    ${VST3_SDK_DIR}/base/source/fobject.cpp
    ${VST3_SDK_DIR}/base/source/fstring.cpp
    ${VST3_SDK_DIR}/base/source/fbuffer.cpp
    ${VST3_SDK_DIR}/base/source/fstreamer.cpp
    ${VST3_SDK_DIR}/base/source/updatehandler.cpp
    ${VST3_SDK_DIR}/base/source/baseiids.cpp
    ${VST3_SDK_DIR}/base/thread/source/flock.cpp
    ${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp
    ${VST3_SDK_DIR}/public.sdk/source/common/commonstringconvert.cpp
    ${VST3_SDK_DIR}/public.sdk/source/common/threadchecker_win32.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/vstinitiids.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_win32.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/parameterchanges.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/connectionproxy.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/eventlist.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/processdata.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp
)

add_library(Vst3HostNative SHARED
    src/vst3_host.cpp
    include/vst3_host_c.h
    ${SDK_SOURCES}
)

target_include_directories(Vst3HostNative
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${VST3_SDK_DIR}
)

target_compile_definitions(Vst3HostNative PRIVATE
    VST3HOSTNATIVE_EXPORTS
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    $<$<CONFIG:Release>:RELEASE=1>
    $<$<CONFIG:Debug>:DEVELOPMENT=1>
)

if(MSVC)
    target_compile_options(Vst3HostNative PRIVATE /W3 /EHsc /MP)
    # The SDK still uses a few deprecated CRT calls.
    target_compile_definitions(Vst3HostNative PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

target_link_libraries(Vst3HostNative PRIVATE ole32 shell32 user32)

# Native-only smoke test: exercises the C ABI with no CLR involved, so a failure can
# be attributed to the host rather than to marshalling. Build Debug to get SDK asserts.
add_executable(Vst3HostSmoke test/smoke.cpp)
target_link_libraries(Vst3HostSmoke PRIVATE Vst3HostNative)
target_include_directories(Vst3HostSmoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(Vst3HostSmoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(Vst3HostNative PROPERTIES
    OUTPUT_NAME "Vst3HostNative"
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)
