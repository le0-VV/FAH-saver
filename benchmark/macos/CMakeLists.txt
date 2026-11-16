add_executable(fah_engine_comparison MACOSX_BUNDLE EngineComparison.mm)
target_compile_options(fah_engine_comparison PRIVATE -fobjc-arc -Wall -Wextra)
target_link_libraries(fah_engine_comparison PRIVATE fah_metal fah_web
    "-framework AppKit" "-framework WebKit" "-framework MetalKit" "-framework Metal" "-framework QuartzCore")
set_target_properties(fah_engine_comparison PROPERTIES OUTPUT_NAME FAHEngineComparison
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ComparisonInfo.plist.in")
set(FAH_COMPARISON_ASSETS)
foreach(asset IN ITEMS saver.html saver.css capture.js scene.js gpu-timer.js renderer.js saver.js)
    list(APPEND FAH_COMPARISON_ASSETS "${PROJECT_SOURCE_DIR}/benchmark/web/${asset}")
endforeach()
set_property(TARGET fah_engine_comparison APPEND PROPERTY LINK_DEPENDS ${FAH_COMPARISON_ASSETS})
add_custom_command(TARGET fah_engine_comparison POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_DIR:fah_engine_comparison>/Contents/Resources/web"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${FAH_COMPARISON_ASSETS}
        "$<TARGET_BUNDLE_DIR:fah_engine_comparison>/Contents/Resources/web"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/third_party/nlohmann/LICENSE.MIT"
        "$<TARGET_BUNDLE_DIR:fah_engine_comparison>/Contents/Resources/nlohmann-json-LICENSE.MIT")
if(BUILD_TESTING)
    add_test(NAME engine_comparison_config COMMAND fah_engine_comparison --self-test-config)
    add_test(NAME engine_comparison_help COMMAND fah_engine_comparison --help)
    set_tests_properties(engine_comparison_config PROPERTIES TIMEOUT 15)
endif()
if(NOT TARGET fah_metal OR NOT TARGET fah_web)
    message(FATAL_ERROR "The comparison app requires the Metal and Web renderer targets")
endif()

