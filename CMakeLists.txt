cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(ENABLE_FORMAT_CHECK "Fail build if source formatting is incorrect" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SRT REQUIRED srt)
else()
  message(STATUS "PkgConfig not found, searching for SRT directly...")
  find_path(SRT_INCLUDE_DIRS srt/srt.h
    PATHS ${CMAKE_PREFIX_PATH}
    PATH_SUFFIXES include
  )
  find_library(SRT_LIBRARIES srt
    PATHS ${CMAKE_PREFIX_PATH}
    PATH_SUFFIXES lib lib64
  )
  if(NOT SRT_INCLUDE_DIRS OR NOT SRT_LIBRARIES)
    message(FATAL_ERROR "SRT not found via PkgConfig or direct search. Provide SRT_INCLUDE_DIRS/SRT_LIBRARIES.")
  endif()
  message(STATUS "Found SRT: ${SRT_LIBRARIES}")
endif()
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${SRT_LIBRARIES})
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${SRT_INCLUDE_DIRS})

if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32)
endif()

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.cpp)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(ENABLE_QT)
  add_executable(obs-srt-meeting-client src/client/main.cpp src/client/mainwindow.cpp src/client/mainwindow.h)
  target_link_libraries(obs-srt-meeting-client PRIVATE Qt6::Core Qt6::Widgets)
  set_target_properties(
    obs-srt-meeting-client
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

if(ENABLE_FORMAT_CHECK AND NOT WIN32)
  set(FORMAT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin-main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/mainwindow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/mainwindow.h
  )
  add_custom_target(check-format
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/build-aux/run-clang-format
      --check --fail-on-change ${FORMAT_SOURCES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Checking C++ formatting with clang-format..."
  )
  add_dependencies(${CMAKE_PROJECT_NAME} check-format)
  if(TARGET obs-srt-meeting-client)
    add_dependencies(obs-srt-meeting-client check-format)
  endif()
endif()
