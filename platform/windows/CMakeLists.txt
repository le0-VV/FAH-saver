if(NOT WIN32)
    return()
endif()

find_package(Vulkan REQUIRED)
find_program(FAH_GLSLANG_VALIDATOR NAMES glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)

add_executable(fah_saver_windows WIN32 Main.cpp Renderer.cpp)
set_target_properties(fah_saver_windows PROPERTIES OUTPUT_NAME FAHSaver SUFFIX ".scr")
target_compile_features(fah_saver_windows PRIVATE cxx_std_17)
target_compile_definitions(fah_saver_windows PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0a00
    FAH_SAVER_VERSION="${PROJECT_VERSION}")
target_link_libraries(fah_saver_windows PRIVATE fah_core fah_source Vulkan::Vulkan user32 gdi32 shell32 advapi32 psapi)
if(MSVC)
    target_compile_options(fah_saver_windows PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(fah_saver_windows PRIVATE -Wall -Wextra)
    target_link_options(fah_saver_windows PRIVATE -municode -Wl,--subsystem,windows)
endif()

set(FAH_SHADERS)
foreach(shader IN ITEMS model.vert model.frag background.vert background.frag)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/compiled-shaders/${shader}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/compiled-shaders"
        COMMAND "${FAH_GLSLANG_VALIDATOR}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}" -o "${output}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}" VERBATIM)
    list(APPEND FAH_SHADERS "${output}")
endforeach()
add_custom_target(fah_windows_shaders DEPENDS ${FAH_SHADERS})
add_dependencies(fah_saver_windows fah_windows_shaders)
add_custom_command(TARGET fah_saver_windows POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_BINARY_DIR}/compiled-shaders" "$<TARGET_FILE_DIR:fah_saver_windows>/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:fah_saver_windows>" "$<TARGET_FILE_DIR:fah_saver_windows>/fah-preview.exe")
add_custom_command(TARGET fah_saver_windows POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/third_party/nlohmann/LICENSE.MIT"
        "$<TARGET_FILE_DIR:fah_saver_windows>/nlohmann-json-LICENSE.MIT")

if(BUILD_TESTING)
    add_executable(fah_windows_options_tests tests/OptionsTests.cpp)
    target_compile_features(fah_windows_options_tests PRIVATE cxx_std_17)
    add_test(NAME windows_options COMMAND fah_windows_options_tests)
    add_executable(fah_windows_winding_tests tests/WindingTests.cpp)
    target_link_libraries(fah_windows_winding_tests PRIVATE fah_core)
    add_test(NAME windows_winding COMMAND fah_windows_winding_tests)
endif()

install(TARGETS fah_saver_windows RUNTIME DESTINATION .)
install(FILES ${FAH_SHADERS} DESTINATION shaders)
install(FILES "${PROJECT_SOURCE_DIR}/third_party/nlohmann/LICENSE.MIT" DESTINATION licences RENAME nlohmann-json-LICENSE.MIT)
