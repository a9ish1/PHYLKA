# PHYLKA (PLK) node core. Build:  cmake -S . -B build && cmake --build build -j
cmake_minimum_required(VERSION 3.20)
project(phylka C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# libsecp256k1 (Bitcoin Core's) with Schnorr signatures
set(SECP256K1_ENABLE_MODULE_SCHNORRSIG ON CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_EXTRAKEYS ON CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ECDH OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_RECOVERY OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ELLSWIFT OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXHAUSTIVE_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SECP256K1_DISABLE_SHARED ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/secp256k1 EXCLUDE_FROM_ALL)

# ML-DSA-44 (FIPS 204) reference implementation and SHA-2/SHA-3 from PQClean.
# randombytes is provided by src/crypto.cpp (OS randomness, or a fixed seed during key derivation).
file(GLOB MLDSA_SRC deps/mldsa44/*.c)
add_library(pqc STATIC ${MLDSA_SRC} deps/pqcommon/fips202.c deps/pqcommon/sha2.c)
target_include_directories(pqc PUBLIC deps/mldsa44 deps/pqcommon)

add_library(plkcore STATIC src/platform.cpp src/crypto.cpp src/keys.cpp src/address.cpp src/chain.cpp src/ext.cpp src/node.cpp src/net.cpp src/wallet.cpp src/explorer.cpp src/gui.cpp ${CMAKE_BINARY_DIR}/gui_assets.cpp)
include(cmake/embed.cmake)
plk_embed_gui(${CMAKE_BINARY_DIR}/gui_assets.cpp
  "/|${CMAKE_SOURCE_DIR}/src/gui/index.html|text/html"
  "/app.js|${CMAKE_SOURCE_DIR}/src/gui/app.js|text/javascript"
  "/app.css|${CMAKE_SOURCE_DIR}/src/gui/app.css|text/css")
target_include_directories(plkcore PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(plkcore PUBLIC secp256k1 pqc Threads::Threads)
option(PLK_PORTABLE_SHA "Use PQClean SHA-256 everywhere" OFF)
if(PLK_PORTABLE_SHA)
  target_compile_definitions(plkcore PUBLIC PLK_PORTABLE_SHA)
endif()
if(WIN32)
  target_link_libraries(plkcore PUBLIC ws2_32 bcrypt)
endif()
if(MSVC)
  target_compile_options(plkcore PRIVATE /W3)
else()
  target_compile_options(plkcore PRIVATE -Wall -Wextra)
endif()

add_executable(phylka src/main.cpp)
target_link_libraries(phylka PRIVATE plkcore)
# Genesis mining tool for private networks: not part of the default build, so the main genesis cannot be replaced by accident
option(PLK_ENABLE_MKGENESIS "Build the 'mkgenesis' subcommand" OFF)
if(PLK_ENABLE_MKGENESIS)
  target_compile_definitions(phylka PRIVATE PLK_ENABLE_MKGENESIS)
endif()

option(PLK_BUILD_BENCH "Build the PIP-0001 block benchmark" OFF)
if(PLK_BUILD_BENCH)
  add_executable(bench-pip1 tests/bench-pip1.cpp)
  target_include_directories(bench-pip1 PRIVATE src)
  target_link_libraries(bench-pip1 PRIVATE plkcore)
endif()
