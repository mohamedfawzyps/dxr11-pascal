cmake_minimum_required(VERSION 3.20)
project(dxr11_pascal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Phase 1: DXIL container re-signing test (Windows / x64).
#
# Needs the DXC SDK headers (dxcapi.h) and, at runtime, dxcompiler.dll and
# dxil.dll from a recent DirectXShaderCompiler release. Point DXC_SDK_DIR at an
# unpacked release (containing inc/ and lib/x64/) or install it via NuGet
# (Microsoft.Direct3D.DXC / Microsoft.Direct3D.D3D12) and set the path.
set(DXC_SDK_DIR "" CACHE PATH "Path to DirectXShaderCompiler SDK (has inc/ and lib/x64/)")

add_executable(signtest src/signtest.cpp)

if(DXC_SDK_DIR)
    target_include_directories(signtest PRIVATE "${DXC_SDK_DIR}/inc")
    target_link_directories(signtest PRIVATE "${DXC_SDK_DIR}/lib/x64")
endif()

# We load dxcompiler.dll / dxil.dll dynamically (LoadLibrary), so no import lib
# is strictly required; link dxcompiler only if you prefer static binding.
