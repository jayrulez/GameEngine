# Raptor third-party dependencies.

# SDL3 — pre-built development libraries (Windows). Point find_package(SDL3)
# to the vendored copy and store the DLL path for post-build copies.
if(WIN32 AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/SDL3/cmake/SDL3Config.cmake)
    set(SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SDL3/cmake" CACHE PATH "SDL3 CMake config" FORCE)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/SDL3/lib/x64/SDL3.dll)
        set(RAPTOR_SDL3_DLL "${CMAKE_CURRENT_SOURCE_DIR}/SDL3/lib/x64/SDL3.dll" CACHE FILEPATH "" FORCE)
    endif()
endif()

# DXC — vendored headers + pre-built runtime binaries (HLSL -> SPIR-V/DXIL).
# Headers are an INTERFACE target; the runtime library is loaded at run time
# (dlopen / LoadLibrary), so we only publish its path + an rpath for dev builds.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/DXC/include/dxc/dxcapi.h)
    add_library(raptor_dxc_headers INTERFACE)
    add_library(Raptor::DXCHeaders ALIAS raptor_dxc_headers)
    target_include_directories(raptor_dxc_headers
        INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/DXC/include)

    if(WIN32)
        set(_dxc_lib_dir "${CMAKE_CURRENT_SOURCE_DIR}/DXC/lib/win-x64")
        if(EXISTS "${_dxc_lib_dir}/dxcompiler.dll")
            target_compile_definitions(raptor_dxc_headers
                INTERFACE RAPTOR_DXC_PATH="${_dxc_lib_dir}/dxcompiler.dll")
            message(STATUS "DXC runtime: ${_dxc_lib_dir}")
        endif()
    elseif(UNIX AND NOT APPLE)
        set(_dxc_lib_dir "${CMAKE_CURRENT_SOURCE_DIR}/DXC/lib/linux-x86_64")
        if(EXISTS "${_dxc_lib_dir}/libdxcompiler.so")
            target_compile_definitions(raptor_dxc_headers
                INTERFACE RAPTOR_DXC_PATH="${_dxc_lib_dir}/libdxcompiler.so")
            list(APPEND CMAKE_BUILD_RPATH "${_dxc_lib_dir}")
            set(CMAKE_BUILD_RPATH "${CMAKE_BUILD_RPATH}" PARENT_SCOPE)
            message(STATUS "DXC runtime: ${_dxc_lib_dir}")
        endif()
    endif()
else()
    message(STATUS "ThirdParty/DXC not present — raptor.shaders will be skipped.")
endif()

# stb / cgltf / ufbx — vendored header(+source) libraries (copied, not submodules).
# Header include-dir INTERFACE targets; the *_impl translation units that define
# the implementations live in the engine libraries that use them.
add_library(raptor_stb INTERFACE)
add_library(Raptor::Stb ALIAS raptor_stb)
target_include_directories(raptor_stb SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/stb)

add_library(raptor_cgltf INTERFACE)
add_library(Raptor::Cgltf ALIAS raptor_cgltf)
target_include_directories(raptor_cgltf SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/cgltf)

add_library(raptor_ufbx INTERFACE)
add_library(Raptor::Ufbx ALIAS raptor_ufbx)
target_include_directories(raptor_ufbx SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/ufbx)

# msdfgen — multi-channel signed distance field generator (compiled from source).
# Import-time only: used by the font atlas baker, not linked at runtime.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/msdfgen/CMakeLists.txt)
    set(MSDFGEN_CORE_ONLY          ON  CACHE BOOL "" FORCE)
    set(MSDFGEN_BUILD_STANDALONE   OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_USE_OPENMP         OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_USE_SKIA           OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_USE_VCPKG          OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_INSTALL            OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_DYNAMIC_RUNTIME    ON  CACHE BOOL "" FORCE)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    add_subdirectory(msdfgen)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
    add_library(Raptor::MsdfgenCore ALIAS msdfgen-core)
endif()
