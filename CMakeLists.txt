cmake_minimum_required(VERSION 3.16)
project(sonycam CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Path to an unpacked Sony Camera Remote SDK (download it yourself from
# https://support.d-imaging.sony.co.jp/app/sdk/en/index.html - it is
# license-gated and must not be committed to this repository).
# Expected layout: ${SONY_SDK_DIR}/CRSDK/*.h and the SDK libraries
# (libCr_Core.so / libCr_Core.dylib) somewhere under ${SONY_SDK_DIR}.
set(SONY_SDK_DIR "" CACHE PATH "Path to the unpacked Sony Camera Remote SDK")

add_library(sonycam_core STATIC
  src/fake_backend.cpp
  src/crsdk_backend.cpp
)
target_include_directories(sonycam_core PUBLIC src third_party)

if(SONY_SDK_DIR)
  find_library(CR_CORE_LIB NAMES Cr_Core
    PATHS ${SONY_SDK_DIR} PATH_SUFFIXES lib external/crsdk
    NO_DEFAULT_PATH)
  if(NOT CR_CORE_LIB)
    message(FATAL_ERROR "SONY_SDK_DIR set but libCr_Core not found under ${SONY_SDK_DIR}")
  endif()
  target_compile_definitions(sonycam_core PRIVATE SONYCAM_WITH_CRSDK)
  target_include_directories(sonycam_core PRIVATE ${SONY_SDK_DIR})
  target_link_libraries(sonycam_core PUBLIC ${CR_CORE_LIB})
  message(STATUS "Building with Sony Camera Remote SDK: ${CR_CORE_LIB}")
else()
  message(STATUS "SONY_SDK_DIR not set - building fake backend only")
endif()

find_package(Threads REQUIRED)

add_executable(sonycamd src/daemon.cpp)
target_link_libraries(sonycamd PRIVATE sonycam_core Threads::Threads)

add_executable(sonycam src/cli.cpp)
target_link_libraries(sonycam PRIVATE sonycam_core Threads::Threads)

enable_testing()
add_test(NAME cli_fake_mode
  COMMAND ${CMAKE_SOURCE_DIR}/tests/run_tests.sh $<TARGET_FILE:sonycam>)
