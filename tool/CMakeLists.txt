# MealKit Audio authoring tool (MealKitAudio): SDL3 window + Dear ImGui (docking) + the mkaudio runtime. Build output goes to <build folder>/bin/
# with the executable and the manifest mealkit-tool.txt together → put that folder in %LOCALAPPDATA%\MealKit\tools\audio\, or
# open the MealKit editor with --tool-path <build folder>/bin and it appears in the [Tools] menu. cmake --install makes the same folder shape.

# SDL3 (static, without tests/examples)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/SDL ${CMAKE_CURRENT_BINARY_DIR}/SDL EXCLUDE_FROM_ALL)

set(imgui_dir ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui)
add_library(mkaudio_imgui STATIC
    ${imgui_dir}/imgui.cpp ${imgui_dir}/imgui_draw.cpp ${imgui_dir}/imgui_tables.cpp ${imgui_dir}/imgui_widgets.cpp
    ${imgui_dir}/backends/imgui_impl_sdl3.cpp ${imgui_dir}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(mkaudio_imgui PUBLIC ${imgui_dir} ${imgui_dir}/backends)
target_link_libraries(mkaudio_imgui PUBLIC SDL3::SDL3-static)

add_executable(MealKitAudio WIN32 main.cpp App.h App.cpp Live.h Live.cpp Json.h)
target_link_libraries(MealKitAudio PRIVATE mkaudio mkaudio_imgui SDL3::SDL3-static)
if(WIN32)
    target_link_libraries(MealKitAudio PRIVATE ws2_32)
endif()
if(MSVC)
    target_compile_options(MealKitAudio PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(MealKitAudio PRIVATE -Wall -Wextra)
endif()
set_target_properties(MealKitAudio PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${config} upper)
    set_target_properties(MealKitAudio PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${upper} ${CMAKE_BINARY_DIR}/bin)
endforeach()

# Manifest (version from project(mkaudio VERSION ...)). exe is written without an extension, .exe is appended on Windows
set(MKAUDIO_VERSION ${PROJECT_VERSION})
configure_file(mealkit-tool.txt ${CMAKE_BINARY_DIR}/bin/mealkit-tool.txt @ONLY)
if(WIN32)
    file(READ ${CMAKE_BINARY_DIR}/bin/mealkit-tool.txt manifest)
    string(REPLACE "exe MealKitAudio\n" "exe MealKitAudio.exe\n" manifest "${manifest}")
    file(WRITE ${CMAKE_BINARY_DIR}/bin/mealkit-tool.txt "${manifest}")
endif()

install(TARGETS MealKitAudio RUNTIME DESTINATION .)
install(FILES ${CMAKE_BINARY_DIR}/bin/mealkit-tool.txt DESTINATION .)
