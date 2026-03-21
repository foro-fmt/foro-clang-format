cmake_minimum_required(VERSION 3.30)
project(foro-clang-format VERSION 22.1.1)

set(CMAKE_CXX_STANDARD 20)

add_library(foro-clang-format SHARED src/main.cpp src/lib.cpp)

include(FetchContent)

set(LLVM_ENABLE_PROJECTS clang CACHE STRING "LLVM projects to build")
set(LLVM_INCLUDE_UTILS OFF CACHE BOOL "Include LLVM utils")
set(LLVM_INCLUDE_EXAMPLES OFF CACHE BOOL "Include LLVM examples")
set(LLVM_INCLUDE_TESTS OFF CACHE BOOL "Include LLVM tests")
set(LLVM_INCLUDE_BENCHMARKS OFF CACHE BOOL "Include LLVM benchmarks")

add_definitions(-fno-rtti)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "MinSizeRel")
endif()

set(LLVM_VERSION "22.1.1")

FetchContent_Declare(
        llvm_project
        URL "https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/llvm-project-${LLVM_VERSION}.src.tar.xz"
        URL_HASH SHA256=9c6f37f6f5f68d38f435d25f770fc48c62d92b2412205767a16dac2c942f0c95
        TLS_VERIFY TRUE
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
)

FetchContent_MakeAvailable(llvm_project json)

add_subdirectory(${llvm_project_SOURCE_DIR}/llvm)

set(LLVM_INCLUDE_DIRS
        ${LLVM_SOURCE_DIR}/include
        ${LLVM_BINARY_DIR}/include
        ${LLVM_EXTERNAL_CLANG_SOURCE_DIR}/include
        ${LLVM_BINARY_DIR}/tools/clang/include
)

set(LLVM_LIBRARIES
        clangBasic
        clangFormat
        clangRewrite
        clangToolingCore
        LLVMSupport
        LLVMCore
        LLVMOption
        LLVMMC
)

target_include_directories(foro-clang-format PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_features(foro-clang-format PRIVATE cxx_std_20)
target_compile_options(foro-clang-format PRIVATE -O3)

set_target_properties(foro-clang-format PROPERTIES OUTPUT_NAME "foro-clang-format")

target_link_libraries(foro-clang-format PRIVATE
        nlohmann_json::nlohmann_json
        ${LLVM_LIBRARIES}
)
