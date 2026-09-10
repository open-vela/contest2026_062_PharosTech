# SPDX-License-Identifier: Apache-2.0
if(CONFIG_NYABULA_EYE_ENGINE)
  if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/src/generated/fonts/font-manifest.json)
    message(FATAL_ERROR
      "Prepare Eye fonts: python3 app/nyabula/tools/generate_fonts.py --download-fallback (in the contest repository)")
  endif()
  file(GLOB NYABULA_FONT_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/generated/fonts/*.c)
  nuttx_add_library(nyabula_eye STATIC
    src/nyabula_eye_engine.c src/nyabula_eye_renderer_lvgl.c
    src/nyabula_core.cxx src/nyabula_eye_service.c src/nyabula_eye_json.c
    src/generated/nyabula_eye_icons.c ${NYABULA_FONT_SRCS})
  target_include_directories(nyabula_eye PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
  target_link_libraries(nyabula_eye PUBLIC lvgl)
  set_source_files_properties(src/nyabula_core.cxx PROPERTIES
    COMPILE_OPTIONS "-std=c++17;-fno-exceptions;-fno-rtti")

  if(CONFIG_NYABULA_EYE_DISPLAY)
    nuttx_add_application(
      NAME nyabula_eye
      PRIORITY ${CONFIG_NYABULA_EYE_PRIORITY}
      STACKSIZE ${CONFIG_NYABULA_EYE_STACKSIZE}
      SRCS src/nyabula_eye_main.c
      INCLUDE_DIRECTORIES ${NUTTX_APPS_DIR}/graphics/nyabula_display
      DEPENDS nyabula_eye apps_nyabula_display lvgl)
  endif()
endif()
