cmake_minimum_required(VERSION 3.31)

project(IonCachyEngin)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# set(LIB_SOURCES )
set(SOURCES src/main.cpp)

# add_library(IonCachyEngin-dev STATIC ${LIB_SOURCES})

# add_library(IonCachyEngin SHARED ${LIB_SOURCES})

add_executable(testIonCachyEngin ${SOURCES})

# target_link_libraries(testIonCachyEngin PUBLIC )

# target_link_libraries(IonCachyEngin-dev PUBLIC )

# target_link_libraries(IonCachyEngin PUBLIC )
