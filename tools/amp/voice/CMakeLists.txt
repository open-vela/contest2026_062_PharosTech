cmake_minimum_required(VERSION 3.16)
project(nyamp_voice LANGUAGES CXX)

# Wake word (keyword spotting) and owner voiceprint (speaker embedding).
# Both sit on the sherpa-onnx C API that nyampd already links for ASR, so no
# new runtime is added to the compute domain.  Model files are never part of
# this tree; see README.md for sources, licenses and checksums.

set(NYAMP_SHERPA_INCLUDE "" CACHE PATH "Directory containing external c-api.h")
set(NYAMP_SHERPA_LIBRARY "" CACHE FILEPATH "External libsherpa-onnx-c-api.so")
set(NYAMP_MODELS_INCLUDE "" CACHE PATH
    "Optional tools/amp/models directory; shares nyamp::models::Status")
option(NYAMP_VOICE_EVAL "Build the host-only synthetic evidence generator" OFF)

if(NOT NYAMP_SHERPA_INCLUDE OR NOT NYAMP_SHERPA_LIBRARY)
  message(FATAL_ERROR
          "Set NYAMP_SHERPA_INCLUDE and NYAMP_SHERPA_LIBRARY (sherpa-onnx 1.13.8)")
endif()

add_library(nyamp_voice STATIC nyamp_kws.cpp nyamp_speaker.cpp)
target_include_directories(nyamp_voice PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                                       PRIVATE ${NYAMP_SHERPA_INCLUDE})
target_compile_features(nyamp_voice PUBLIC cxx_std_17)
target_compile_options(nyamp_voice PRIVATE -Wall -Wextra -Werror)
target_link_libraries(nyamp_voice PUBLIC ${NYAMP_SHERPA_LIBRARY})
if(NYAMP_MODELS_INCLUDE)
  target_include_directories(nyamp_voice PUBLIC ${NYAMP_MODELS_INCLUDE})
  target_compile_definitions(nyamp_voice PUBLIC NYAMP_VOICE_WITH_MODELS=1)
endif()

foreach(test nyamp_kws_file_test nyamp_speaker_file_test)
  add_executable(${test} ${test}.cpp)
  target_include_directories(${test} PRIVATE ${NYAMP_SHERPA_INCLUDE})
  target_link_libraries(${test} PRIVATE nyamp_voice)
  target_compile_options(${test} PRIVATE -Wall -Wextra -Werror)
endforeach()

if(NYAMP_VOICE_EVAL)
  add_executable(nyamp_voice_tts_batch eval/nyamp_voice_tts_batch.cpp)
  target_include_directories(nyamp_voice_tts_batch PRIVATE ${NYAMP_SHERPA_INCLUDE})
  target_link_libraries(nyamp_voice_tts_batch PRIVATE ${NYAMP_SHERPA_LIBRARY})
  # The evidence generator uses the simple deprecated one-shot TTS call.
  target_compile_options(nyamp_voice_tts_batch PRIVATE -Wall -Wextra
                         -Wno-deprecated-declarations)
endif()
