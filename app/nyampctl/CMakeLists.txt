if(CONFIG_EXAMPLES_NYAMPCTL)
  get_filename_component(NYAMPCTL_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}" REALPATH)
  set(NYAMP_PROTOCOL_DIR
      "${NYAMPCTL_SOURCE_DIR}/../../tools/amp/protocol")
  # With the Nyabula Core compute service in the image the endpoint belongs
  # to that service, and this diagnostic talks through it (nyampctl_io.h).
  set(NYAMPCTL_COMPUTE_DIR)
  if(CONFIG_NYABULA_CORE_COMPUTE)
    set(NYAMPCTL_COMPUTE_DIR "${NYAMPCTL_SOURCE_DIR}/../nyabula_core")
  endif()
  # The kws/asr/tts commands drive the client and the audio nodes the Core
  # voice service is built from; in a flat image those objects are its.
  set(NYAMPCTL_VOICE_SRCS)
  if(CONFIG_NYABULA_CORE_VOICE)
    set(NYAMPCTL_VOICE_SRCS nyampctl_voice.c)
  endif()
  nuttx_add_application(
    MODULE
    ${CONFIG_EXAMPLES_NYAMPCTL}
    NAME
    ${CONFIG_EXAMPLES_NYAMPCTL_PROGNAME}
    SRCS
    nyampctl_main.c
    nyampctl_llm.c
    nyampctl_blob.c
    nyampctl_shmem.c
    ${NYAMPCTL_VOICE_SRCS}
    ${NYAMP_PROTOCOL_DIR}/nyamp_protocol.c
    INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_LIST_DIR}
    ${NYAMP_PROTOCOL_DIR}
    ${NYAMPCTL_COMPUTE_DIR}
    STACKSIZE
    ${CONFIG_EXAMPLES_NYAMPCTL_STACKSIZE}
    PRIORITY
    ${CONFIG_EXAMPLES_NYAMPCTL_PRIORITY})
endif()
