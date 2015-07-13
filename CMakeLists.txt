cmake_minimum_required(VERSION 2.8)

include("yong_injector/injector.cmake")

set(SQLITE_SOURCES
  "amalgamation/sqlite3-all.c"
  )

yong_project(sqlite C)

include_directories("${CMAKE_CURRENT_SOURCE_DIR}/amalgamation"
                    "${CMAKE_CURRENT_BINARY_DIR}/include")

yong_add_library(sqlite SOURCES ${SQLITE_SOURCES})

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/amalgamation/sqlite_config.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/include/sqlite_config.h")

yong_add_library_end(sqlite)

file(GLOB SQLITE_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/amalgamation/*.h")
set(SQLITE_HEADERS
  ${SQLITE_HEADERS} "${CMAKE_CURRENT_BINARY_DIR}/include/sqlite_config.h")
yong_install_header_files(sqlite "" ${SQLITE_HEADERS})

yong_project_end()
