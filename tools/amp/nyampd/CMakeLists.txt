cmake_minimum_required(VERSION 3.16)
project(nyampd C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optional RKLLM runtime.  Without it nyampd still builds and serves health and
# info, but every LLM opcode is answered unsupported.  Point NYAMP_RKLLM_ROOT
# at a directory containing rkllm.h and librkllmrt.so to enable the model
# service; the runtime is never vendored into this repository.
#
# The variable must be set before the models subdirectory is configured, since
# that is where the optional backend target is defined.
set(NYAMP_RKLLM_ROOT "" CACHE PATH "Directory containing external rkllm.h and librkllmrt.so")

add_subdirectory(../protocol protocol)
add_subdirectory(../models models)

add_library(nyampd_core STATIC nyampd_core.cpp nyampd_llm.cpp)
target_include_directories(
  nyampd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../models)
target_link_libraries(nyampd_core PUBLIC nyamp_protocol nyamp_models)
target_compile_options(nyampd_core PRIVATE -Wall -Wextra -Werror)

add_executable(nyampd nyampd_main.cpp)
target_link_libraries(nyampd PRIVATE nyampd_core)
target_compile_options(nyampd PRIVATE -Wall -Wextra -Werror)

# Link the backend only when the models subdirectory actually produced it.
if(TARGET nyamp_rkllm)
  target_link_libraries(nyampd PRIVATE nyamp_rkllm)
  target_compile_definitions(nyampd PRIVATE NYAMP_WITH_RKLLM=1)
endif()

add_executable(nyampd_core_test nyampd_core_test.cpp)
target_link_libraries(nyampd_core_test PRIVATE nyampd_core)
target_compile_options(nyampd_core_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME nyampd_core_test COMMAND nyampd_core_test)
