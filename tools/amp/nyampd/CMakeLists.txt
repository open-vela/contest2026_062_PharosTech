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
# Chat front end (template, tokenizer, output parser).  It stays buildable on
# its own; pulled in as a subdirectory it contributes only its library.
add_subdirectory(../chat chat)
# Text front end for TTS (normalisation, G2P, sentence splitting).  Standard
# library only, so it is always part of the daemon; the 7 MB lexicon it reads
# arrives with the model.
add_subdirectory(../g2p g2p)

# The speech services (ASR, TTS, KWS) are always compiled: they hold no
# reference to a runtime, only to the small backend interfaces, so a daemon
# built without sherpa-onnx / onnxruntime / rknn still links, answers their
# opcodes unsupported and leaves their HEALTH capability bits clear.
add_library(nyampd_core STATIC nyampd_core.cpp nyampd_llm.cpp nyampd_frame.cpp
                               nyampd_sha256.cpp nyampd_blob.cpp
                               nyampd_provision.cpp nyampd_chat.cpp
                               nyampd_audio.cpp nyampd_loader.cpp
                               nyampd_asr.cpp nyampd_tts.cpp nyampd_kws.cpp)
target_include_directories(
  nyampd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../models
                     ${CMAKE_CURRENT_SOURCE_DIR}/../linux)
target_link_libraries(nyampd_core PUBLIC nyamp_protocol nyamp_models
                                         nyamp_chat nyamp_g2p)
target_compile_options(nyampd_core PRIVATE -Wall -Wextra -Werror)

add_executable(nyampd nyampd_main.cpp)
target_link_libraries(nyampd PRIVATE nyampd_core)
# The arena layout is the control domain's header; the daemon grants windows
# out of it, so it reads the same file instead of repeating the offsets.
target_include_directories(
  nyampd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../chips/rk3576/include)
target_compile_options(nyampd PRIVATE -Wall -Wextra -Werror)

# Link the backend only when the models subdirectory actually produced it.
if(TARGET nyamp_rkllm)
  target_link_libraries(nyampd PRIVATE nyamp_rkllm)
  target_compile_definitions(nyampd PRIVATE NYAMP_WITH_RKLLM=1)
endif()

# Speech runtimes, all optional and never vendored.  NYAMP_SHERPA_INCLUDE /
# NYAMP_SHERPA_LIBRARY (sherpa-onnx 1.13.8 C API) give streaming ASR and the
# wake word; NYAMP_ORT_* plus NYAMP_RKNN_* give MeloTTS (CPU prefix + NPU
# vocoder), which only exists for aarch64.  The models subdirectory defines
# nyamp_sherpa / nyamp_melo when the matching variables are set.
if(TARGET nyamp_sherpa)
  # tools/amp/voice shares nyamp::models::Status when it is told where the
  # models layer is; a cache entry because that directory declares it as one.
  set(NYAMP_MODELS_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../models CACHE PATH
      "tools/amp/models, shared with tools/amp/voice" FORCE)
  add_subdirectory(../voice voice)
  add_library(nyampd_kws_sherpa STATIC nyampd_kws_sherpa.cpp)
  target_link_libraries(nyampd_kws_sherpa PUBLIC nyampd_core nyamp_voice)
  target_compile_options(nyampd_kws_sherpa PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(nyampd PRIVATE nyamp_sherpa nyampd_kws_sherpa)
  target_compile_definitions(nyampd PRIVATE NYAMP_WITH_SHERPA=1)
endif()

if(TARGET nyamp_melo)
  target_link_libraries(nyampd PRIVATE nyamp_melo)
  target_compile_definitions(nyampd PRIVATE NYAMP_WITH_MELO=1)
endif()

add_executable(nyampd_core_test nyampd_core_test.cpp)
target_link_libraries(nyampd_core_test PRIVATE nyampd_core)
target_compile_options(nyampd_core_test PRIVATE -Wall -Wextra -Werror)

# Blob delivery against an in-process control-domain responder: reassembly,
# digest and size rejection, short reads, stale generation, cancellation.
add_executable(nyampd_blob_test nyampd_blob_test.cpp)
target_link_libraries(nyampd_blob_test PRIVATE nyampd_core)
target_compile_options(nyampd_blob_test PRIVATE -Wall -Wextra -Werror)

# Chat completions end to end against a scripted backend.  The vocabulary is
# replaced by a byte codec; the chat template and the output parser are the
# real ones.  Set NYAMP_TOKENIZER_JSON in the environment to also run the
# checks that need the model's tokenizer.json (never in the repository).
add_executable(nyampd_chat_test nyampd_chat_test.cpp)
target_link_libraries(nyampd_chat_test PRIVATE nyampd_core)
target_compile_options(nyampd_chat_test PRIVATE -Wall -Wextra -Werror)

# The speech services against scripted backends, through Dispatch and the
# event queues only -- the wire as the control domain sees it.  With the
# sherpa runtime linked, the ASR and KWS tests also run the real models when
# the environment names them (NYAMP_ASR_MODEL + NYAMP_ASR_WAVS,
# NYAMP_KWS_MODEL + NYAMP_KWS_POSITIVE + NYAMP_KWS_NEGATIVE); the TTS test
# runs the real text front end when NYAMP_G2P_ASSETS names its lexicon.
# Model files and audio are never in the repository.
foreach(test nyampd_audio_test nyampd_asr_test nyampd_tts_test nyampd_kws_test)
  add_executable(${test} ${test}.cpp)
  target_link_libraries(${test} PRIVATE nyampd_core)
  target_compile_options(${test} PRIVATE -Wall -Wextra -Werror)
endforeach()

if(TARGET nyamp_sherpa)
  target_include_directories(nyampd_asr_test PRIVATE ${NYAMP_SHERPA_INCLUDE})
  target_link_libraries(nyampd_asr_test PRIVATE nyamp_sherpa)
  target_compile_definitions(nyampd_asr_test PRIVATE NYAMP_WITH_SHERPA=1)
  target_include_directories(nyampd_kws_test PRIVATE ${NYAMP_SHERPA_INCLUDE})
  target_link_libraries(nyampd_kws_test PRIVATE nyamp_sherpa nyampd_kws_sherpa)
  target_compile_definitions(nyampd_kws_test PRIVATE NYAMP_WITH_SHERPA=1)
endif()

enable_testing()
add_test(NAME nyampd_core_test COMMAND nyampd_core_test)
add_test(NAME nyampd_audio_test COMMAND nyampd_audio_test)
add_test(NAME nyampd_asr_test COMMAND nyampd_asr_test)
add_test(NAME nyampd_tts_test COMMAND nyampd_tts_test)
add_test(NAME nyampd_kws_test COMMAND nyampd_kws_test)
add_test(NAME nyampd_chat_test COMMAND nyampd_chat_test)
add_test(NAME nyampd_blob_test COMMAND nyampd_blob_test)

# Self-test for the shared region.  It only touches the driver, so it runs
# standalone: a mapping or geometry disagreement is found here rather than
# inside a transcription.
add_executable(nyamp_shmem_test nyamp_shmem_test.cpp)
target_include_directories(nyamp_shmem_test PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../linux)
target_compile_options(nyamp_shmem_test PRIVATE -Wall -Wextra -Werror)
