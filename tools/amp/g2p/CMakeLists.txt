# tools/amp/g2p/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# MeloTTS zh_en text front end.  The library has no dependency beyond the
# C++17 standard library so that it links into the aarch64 daemon as is.
#
# Host build + test:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j2
#   NYAMP_G2P_ASSETS=/path/to/vits-melo-tts-zh_en \
#   NYAMP_G2P_GOLDEN=/path/to/real-fixture ctest --test-dir build -V
#
# Target compile check (library only, tests cannot run on the host):
#   cmake -S . -B build-arm64 -DCMAKE_SYSTEM_NAME=Linux \
#     -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
#     -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ -DNYAMP_G2P_TOOLS=OFF

cmake_minimum_required(VERSION 3.16)
project(nyamp_g2p CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NYAMP_G2P_TOOLS "Build the nyamp-g2p CLI and the unit test" ON)

add_library(nyamp_g2p STATIC nyamp_g2p.cpp)
target_include_directories(nyamp_g2p PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(nyamp_g2p PRIVATE -Wall -Wextra -Werror)

if(NYAMP_G2P_TOOLS)
  add_executable(nyamp-g2p nyamp_g2p_main.cpp)
  target_link_libraries(nyamp-g2p PRIVATE nyamp_g2p)
  target_compile_options(nyamp-g2p PRIVATE -Wall -Wextra -Werror)

  add_executable(nyamp_g2p_test nyamp_g2p_test.cpp)
  target_link_libraries(nyamp_g2p_test PRIVATE nyamp_g2p)
  target_compile_options(nyamp_g2p_test PRIVATE -Wall -Wextra -Werror)

  # The model assets are third-party files that are not in the repository
  # (see README.md).  The test reads NYAMP_G2P_ASSETS / NYAMP_G2P_GOLDEN
  # from the environment and exits with 77 when the assets are missing, so
  # CTest shows "skipped" rather than a pass that checked nothing.
  enable_testing()
  add_test(NAME nyamp_g2p_test COMMAND nyamp_g2p_test)
  set_tests_properties(nyamp_g2p_test PROPERTIES SKIP_RETURN_CODE 77)
endif()
