# MealKit Audio 저작 도구 (MealKitAudio): SDL3 창 + Dear ImGui(도킹) + 런타임 mkaudio. 빌드 결과는 <빌드 폴더>/bin/에
# 실행 파일과 매니페스트 mealkit-tool.txt가 같이 놓인다 → 그 폴더를 %LOCALAPPDATA%\MealKit\tools\audio\에 넣거나,
# MealKit 에디터를 --tool-path <빌드 폴더>/bin로 열면 [도구] 메뉴에 나온다. cmake --install로도 같은 폴더 모양을 만든다.

# SDL3 (정적, 시험·예제 없이)
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

# 매니페스트 (버전은 project(mkaudio VERSION ...)). exe는 확장자 없이 적고 Windows에서는 .exe를 붙인다
set(MKAUDIO_VERSION ${PROJECT_VERSION})
configure_file(mealkit-tool.txt ${CMAKE_BINARY_DIR}/bin/mealkit-tool.txt @ONLY)
if(WIN32)
    file(READ ${CMAKE_BINARY_DIR}/bin/mealkit-tool.txt manifest)
    string(REPLACE "exe MealKitAudio\n" "exe MealKitAudio.exe\n" manifest "${manifest}")
    file(WRITE ${CMAKE_BINARY_DIR}/bin/mealkit-tool.txt "${manifest}")
endif()

install(TARGETS MealKitAudio RUNTIME DESTINATION .)
install(FILES ${CMAKE_BINARY_DIR}/bin/mealkit-tool.txt DESTINATION .)
