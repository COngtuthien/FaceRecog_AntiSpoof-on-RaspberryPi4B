CMAKE_MINIMUM_REQUIRED(VERSION 3.6)

PROJECT(LiveFaceReco)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

find_package(OpenCV REQUIRED)
find_package(OpenMP REQUIRED)

SET(CMAKE_CXX_STANDARD 17)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)

SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")

# Bundled ncnn inside this repo for Raspberry Pi Linux aarch64
SET(NCNN_ROOT "${PROJECT_SOURCE_DIR}/include")
SET(NCNN_LIB "${NCNN_ROOT}/libs/RaspberryPi4B/libncnn.a")
SET(NCNN_INCLUDE_DIR "${NCNN_ROOT}/ncnn")

if(NOT EXISTS ${NCNN_LIB})
    message(FATAL_ERROR "Cannot find native ncnn library: ${NCNN_LIB}")
endif()

if(NOT EXISTS ${NCNN_INCLUDE_DIR})
    message(FATAL_ERROR "Cannot find native ncnn include dir: ${NCNN_INCLUDE_DIR}")
endif()

message(STATUS "Using NCNN_ROOT = ${NCNN_ROOT}")
message(STATUS "Using NCNN_LIB  = ${NCNN_LIB}")

INCLUDE_DIRECTORIES(${OpenCV_INCLUDE_DIRS})
INCLUDE_DIRECTORIES(${PROJECT_SOURCE_DIR}/include)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_LIST_DIR}/src)
INCLUDE_DIRECTORIES(${NCNN_INCLUDE_DIR})

if(OPENMP_FOUND)
    message("OPENMP FOUND")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${OpenMP_EXE_LINKER_FLAGS}")
endif()

ADD_LIBRARY(
    facedetect STATIC
    include/FacePreprocess.h
    src/livefacereco.hpp
    src/livefacereco.cpp
    src/live.cpp
    src/live.h
    src/arcface.cpp
    src/arcface.h
    src/mtcnn_new.cpp
    src/mtcnn_new.h

)

# add_subdirectory(src)

# if(${BUILD_TESTS})
#     include(${CMAKE_MODULE_PATH}/Tests.cmake)
# endif()

ADD_EXECUTABLE(
    LiveFaceReco
    src/main.cpp
)

TARGET_LINK_LIBRARIES(
    LiveFaceReco
    facedetect
    ${NCNN_LIB}
    ${OpenCV_LIBS}
    pthread
    m
    dl
    stdc++fs
)

TARGET_LINK_LIBRARIES(
    facedetect
    ${NCNN_LIB}
    ${OpenCV_LIBS}
    pthread
    m
    dl
)

TARGET_INCLUDE_DIRECTORIES(
    LiveFaceReco PRIVATE
    ${OpenCV_INCLUDE_DIRS}
)