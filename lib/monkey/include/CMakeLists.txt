if(NOT BUILD_LOCAL)
  install(DIRECTORY
      ${CMAKE_CURRENT_SOURCE_DIR}/monkey/
    DESTINATION
      ${MK_PATH_HEADERS})
endif()
