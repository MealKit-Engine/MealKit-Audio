# mkaudio: sound event runtime (MealKit Audio). Built standalone it includes tests and the authoring tool (tool/); added by another project with add_subdirectory, only the library.
#   cmake -S . -B build && cmake --build build && ctest --test-dir build
# Values the including side can set (defaults to this repo's third_party/):
#   MKAUDIO_MINIAUDIO_DIR   folder containing miniaudio.h
#   MKAUDIO_STB_DIR         folder containing stb_vorbis.c
cmake_minimum_required(VERSION 3.25)
project(mkaudio VERSION 0.3.1 LANGUAGES C CXX)

if(NOT MKAUDIO_MINIAUDIO_DIR)
    set(MKAUDIO_MINIAUDIO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio)
endif()
if(NOT MKAUDIO_STB_DIR)
    set(MKAUDIO_STB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
endif()
option(MKAUDIO_BUILD_TESTS "Build mkaudio tests" ${PROJECT_IS_TOP_LEVEL})
option(MKAUDIO_BUILD_TOOL "Build the MealKitAudio authoring tool (SDL3 + Dear ImGui)" ${PROJECT_IS_TOP_LEVEL})

add_library(mkaudio STATIC
    include/mkaudio/Mixer.h
    include/mkaudio/Events.h
    src/Mixer.cpp
    src/Events.cpp
    src/MiniaudioConfig.h
    src/MiniaudioImpl.cpp)
target_include_directories(mkaudio PUBLIC include)
target_include_directories(mkaudio SYSTEM PRIVATE ${MKAUDIO_MINIAUDIO_DIR} ${MKAUDIO_STB_DIR})
target_compile_features(mkaudio PUBLIC cxx_std_20)
target_compile_definitions(mkaudio PUBLIC MKAUDIO_VERSION="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(mkaudio PRIVATE /permissive- /utf-8 /Zc:__cplusplus)
    set_source_files_properties(src/Mixer.cpp src/Events.cpp PROPERTIES COMPILE_OPTIONS "/W4")
    set_source_files_properties(src/MiniaudioImpl.cpp PROPERTIES COMPILE_OPTIONS "/W0") # external library implementation
else()
    target_compile_options(mkaudio PRIVATE -Wall -Wextra)
    set_source_files_properties(src/MiniaudioImpl.cpp PROPERTIES COMPILE_OPTIONS "-w")
    find_package(Threads REQUIRED)
    target_link_libraries(mkaudio PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    if(NOT APPLE)
        target_link_libraries(mkaudio PRIVATE m)
    else()
        target_link_libraries(mkaudio PRIVATE "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
    endif()
endif()

if(MKAUDIO_BUILD_TESTS)
    enable_testing()
    add_executable(mkaudio_tests tests/MixerTests.cpp)
    target_link_libraries(mkaudio_tests PRIVATE mkaudio)
    if(MSVC)
        target_compile_options(mkaudio_tests PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(mkaudio_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_test(NAME mkaudio_tests COMMAND mkaudio_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

if(MKAUDIO_BUILD_TOOL)
    add_subdirectory(tool)
endif()
