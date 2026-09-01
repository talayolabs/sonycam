cmake_minimum_required(VERSION 3.16)
project(sonycam VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Path to an unpacked Sony Camera Remote SDK (download it yourself from
# https://support.d-imaging.sony.co.jp/app/sdk/en/index.html - it is
# license-gated and must not be committed to this repository).
# SONY_SDK_DIR may point either at a flat SDK tree or at Sony's extracted
# RemoteCli/SimpleCli sample root (headers in app/CRSDK and libraries in
# external/crsdk).
set(SONY_SDK_DIR "" CACHE PATH "Path to the unpacked Sony Camera Remote SDK")

add_library(sonycam_core STATIC
  src/fake_backend.cpp
  src/crsdk_backend.cpp
)
target_include_directories(sonycam_core PUBLIC src third_party)

if(SONY_SDK_DIR)
  find_path(CRSDK_INCLUDE_DIR NAMES CRSDK/CameraRemote_SDK.h
    PATHS ${SONY_SDK_DIR}
    PATH_SUFFIXES "" app include
    NO_DEFAULT_PATH)
  find_library(CR_CORE_LIB NAMES Cr_Core
    PATHS ${SONY_SDK_DIR} PATH_SUFFIXES lib external/crsdk
    NO_DEFAULT_PATH)
  if(NOT CRSDK_INCLUDE_DIR)
    message(FATAL_ERROR
      "SONY_SDK_DIR set but CRSDK/CameraRemote_SDK.h not found under ${SONY_SDK_DIR}")
  endif()
  if(NOT CR_CORE_LIB)
    message(FATAL_ERROR "SONY_SDK_DIR set but libCr_Core not found under ${SONY_SDK_DIR}")
  endif()
  target_compile_definitions(sonycam_core PRIVATE SONYCAM_WITH_CRSDK)
  target_include_directories(sonycam_core PRIVATE ${CRSDK_INCLUDE_DIR})
  target_link_libraries(sonycam_core PUBLIC ${CR_CORE_LIB})
  if(APPLE)
    find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(sonycam_core PUBLIC
      ${IOKIT_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
  endif()
  get_filename_component(CRSDK_LIB_DIR ${CR_CORE_LIB} DIRECTORY)
  message(STATUS "Building with Sony Camera Remote SDK headers: ${CRSDK_INCLUDE_DIR}")
  message(STATUS "Building with Sony Camera Remote SDK library: ${CR_CORE_LIB}")
else()
  message(STATUS "SONY_SDK_DIR not set - building fake backend only")
endif()

find_package(Threads REQUIRED)

add_executable(sonycamd src/daemon.cpp)
target_link_libraries(sonycamd PRIVATE sonycam_core Threads::Threads)

if(SONY_SDK_DIR)
  if(APPLE)
    # Cr_Core discovers its USB/IP adapters in this bundle-style location,
    # even though sonycamd itself is a command-line executable. This mirrors
    # the layout staged by Sony's macOS sample project.
    set_target_properties(sonycamd PROPERTIES BUILD_RPATH "@executable_path")
    add_custom_command(TARGET sonycamd POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CRSDK_LIB_DIR} $<TARGET_FILE_DIR:sonycamd>
      COMMAND ${CMAKE_COMMAND} -E make_directory
              $<TARGET_FILE_DIR:sonycamd>/Contents/Frameworks/CrAdapter
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              $<TARGET_FILE_DIR:sonycamd>/CrAdapter
              $<TARGET_FILE_DIR:sonycamd>/Contents/Frameworks/CrAdapter
      COMMAND ${CMAKE_COMMAND} -E rm -rf
              $<TARGET_FILE_DIR:sonycamd>/CrAdapter)
  elseif(UNIX)
    set_target_properties(sonycamd PROPERTIES
      BUILD_RPATH "$ORIGIN"
      INSTALL_RPATH "$ORIGIN")
    add_custom_command(TARGET sonycamd POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CRSDK_LIB_DIR} $<TARGET_FILE_DIR:sonycamd>)
  endif()
endif()

add_executable(sonycam src/cli.cpp)
target_link_libraries(sonycam PRIVATE sonycam_core Threads::Threads)
target_compile_definitions(sonycam PRIVATE
  SONYCAM_VERSION="${PROJECT_VERSION}")

# `cmake --install build` puts both binaries on PATH (default /usr/local/bin).
# Real-SDK builds should keep running from the build directory, where the
# Sony dylibs and CrAdapter plugins are staged next to sonycamd.
install(TARGETS sonycam sonycamd RUNTIME DESTINATION bin)

enable_testing()
add_test(NAME cli_fake_mode
  COMMAND ${CMAKE_SOURCE_DIR}/tests/run_tests.sh $<TARGET_FILE:sonycam>)
