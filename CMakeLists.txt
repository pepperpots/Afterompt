cmake_minimum_required(VERSION 3.10)

project(afterompt VERSION 0.2.0 DESCRIPTION "OMPT tool for generating Aftermath traces")

find_package(PkgConfig)

pkg_check_modules(LIBTRACE REQUIRED libaftermath-trace)

set(CMAKE_C_FLAGS_DEBUG "-g -fopenmp")
set(CMAKE_C_FLAGS_RELEASE "-fopenmp")

set(SOURCES
    "src/afterompt.c"
    "src/trace.c"
)

set(COMPILER_DEFS
    ""
)

if(TRACE_LOOPS)
  set(COMPILER_DEFS "${COMPILER_DEFS} -DTRACE_LOOPS")
endif()

if(TRACE_TASKS)
  set(COMPILER_DEFS "${COMPILER_DEFS} -DTRACE_TASKS")
endif()

if(TRACE_OTHERS)
  set(COMPILER_DEFS "${COMPILER_DEFS} -DTRACE_OTHERS")
endif()

if(ALLOW_EXPERIMENTAL)
  set(COMPILER_DEFS "${COMPILER_DEFS} -DALLOW_EXPERIMENTAL")
endif()

if(SUPPORT_TRACE_CALLSTACK)
  set(COMPILER_DEFS "${COMPILER_DEFS} -DSUPPORT_TRACE_CALLSTACK")
endif()

add_definitions(${COMPILER_DEFS})

link_directories(${LIBTRACE_LIBRARY_DIRS})

add_library(${CMAKE_PROJECT_NAME} SHARED ${SOURCES})

set_property(TARGET ${CMAKE_PROJECT_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${LIBTRACE_INCLUDE_DIRS})

target_link_libraries(${CMAKE_PROJECT_NAME} ${LIBTRACE_LIBRARIES})

install(TARGETS ${CMAKE_PROJECT_NAME} DESTINATION ${PROJECT_SOURCE_DIR}/install)

