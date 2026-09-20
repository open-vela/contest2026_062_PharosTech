cmake_minimum_required(VERSION 3.16)
project(nyamp_chat CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Chat front end for the Linux compute domain: OpenAI request -> MiniCPM5 token
# ids, model text -> OpenAI response.  Deliberately free of third-party code
# and of the model's own files: tokenizer.json is a runtime asset that lives
# next to the .rkllm model on the device, never in this repository.
add_library(
  nyamp_chat STATIC
  nyamp_json.cpp
  nyamp_unicode.cpp
  nyamp_tokenizer.cpp
  nyamp_chat_template.cpp
  nyamp_chat_parse.cpp)
target_include_directories(nyamp_chat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(nyamp_chat PRIVATE -Wall -Wextra -Werror)

# Everything below is tooling and tests.  nyampd pulls this directory in with
# add_subdirectory() and only wants the library, so skip the rest then.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_executable(nyamp_chat_cli nyamp_chat_cli.cpp)
  target_link_libraries(nyamp_chat_cli PRIVATE nyamp_chat)
  target_compile_options(nyamp_chat_cli PRIVATE -Wall -Wextra -Werror)

  add_executable(nyamp_chat_unit_test tests/nyamp_chat_unit_test.cpp)
  target_link_libraries(nyamp_chat_unit_test PRIVATE nyamp_chat)
  target_compile_options(nyamp_chat_unit_test PRIVATE -Wall -Wextra -Werror)

  add_executable(nyamp_chat_golden_test tests/nyamp_chat_golden_test.cpp)
  target_link_libraries(nyamp_chat_golden_test PRIVATE nyamp_chat)
  target_compile_options(nyamp_chat_golden_test PRIVATE -Wall -Wextra -Werror)

  enable_testing()
  add_test(NAME nyamp_chat_unit_test COMMAND nyamp_chat_unit_test)

  # The golden test needs the model's tokenizer.json for its id checks.  Pass
  # -DNYAMP_TOKENIZER_JSON=/path/to/tokenizer.json (or export the variable of
  # the same name).  Without it the prompt-rendering and parser round-trip
  # parts still run and ctest reports the test as skipped, not as passed.
  set(NYAMP_TOKENIZER_JSON "" CACHE FILEPATH "MiniCPM5-1B tokenizer.json (not in the repository)")
  set(NYAMP_GOLDEN_JSON "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden_small.json"
      CACHE FILEPATH "Golden file produced by tests/gen_golden.py")
  add_test(NAME nyamp_chat_golden_test
           COMMAND nyamp_chat_golden_test ${NYAMP_GOLDEN_JSON} ${NYAMP_TOKENIZER_JSON})
  set_tests_properties(nyamp_chat_golden_test PROPERTIES SKIP_RETURN_CODE 77)
endif()
