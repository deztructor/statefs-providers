PROJECT(statefs-providers)
cmake_minimum_required(VERSION 2.8)

IF(NOT DEFINED STATEFS_QT_VERSION)
MESSAGE(FATAL_ERROR "Define STATEFS_QT_VERSION")
ENDIF(NOT DEFINED STATEFS_QT_VERSION)

option(ENABLE_MULTIARCH "Enable support for multi-arch distros (lib64)" OFF)
IF(ENABLE_MULTIARCH)
  IF(CMAKE_SIZEOF_VOID_P EQUAL 4)
    SET(LIB_SUFFIX "")
  ELSE(CMAKE_SIZEOF_VOID_P EQUAL 4)
    SET(LIB_SUFFIX 64)
  ENDIF(CMAKE_SIZEOF_VOID_P EQUAL 4)
ELSE()
  SET(LIB_SUFFIX "")
ENDIF()

set(DST_LIB lib${LIB_SUFFIX})

find_package(PkgConfig REQUIRED)

set(CMAKE_CXX_FLAGS
  "${CMAKE_CXX_FLAGS} -Wall -O2 -g -feliminate-unused-debug-types -std=c++0x"
  )

#-Wno-psabi is to remove next g++ warning/note:
#the mangling of 'va_list' has changed in GCC 4.4
set(CMAKE_CXX_FLAGS
  "${CMAKE_CXX_FLAGS} -Wno-psabi"
)

# fix for gcc 4.6 specific compiling/linking issues, no dwarf-4
set(CMAKE_CXX_FLAGS
  "${CMAKE_CXX_FLAGS} -gdwarf-3"
  )

pkg_check_modules(STATEFS statefs-cpp REQUIRED)
pkg_check_modules(STATEFS_QT5 statefs-qt5 REQUIRED)

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${STATEFS_INCLUDES}
  ${STATEFS_QT5_INCLUDES}
)

link_directories(
  ${STATEFS_LIBS}
  ${STATEFS_QT5_LIBS}
)

install(DIRECTORY include/statefs DESTINATION include/qt5)

find_package(Threads)

find_package(Qt5Core REQUIRED)
find_package(Qt5DBus REQUIRED)

set(STATEFS_QT_HEADERS ${CMAKE_SOURCE_DIR}/include/statefs/qt/dbus.hpp)

add_subdirectory(src/util)
add_subdirectory(src/bluez)
add_subdirectory(src/upower)
add_subdirectory(src/connman)
add_subdirectory(src/ofono)
add_subdirectory(src/mce)
add_subdirectory(src/profile)
add_subdirectory(src/keyboard_generic)
add_subdirectory(src/udev)
add_subdirectory(tests)
