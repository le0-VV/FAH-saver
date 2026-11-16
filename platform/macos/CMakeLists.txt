add_library(fah_metal STATIC FAHRenderer.mm)
target_include_directories(fah_metal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(fah_metal PRIVATE -fobjc-arc)
target_link_libraries(fah_metal PUBLIC fah_core
    "-framework AppKit" "-framework Metal" "-framework MetalKit" "-framework QuartzCore")

add_library(fah_web STATIC FAHWebRenderer.mm)
target_include_directories(fah_web PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
target_compile_options(fah_web PRIVATE -fobjc-arc -Wall -Wextra)
target_link_libraries(fah_web PUBLIC fah_core "-framework AppKit" "-framework WebKit")

add_executable(fah_macos_preview Preview.mm)
target_compile_options(fah_macos_preview PRIVATE -fobjc-arc)
target_link_libraries(fah_macos_preview PRIVATE fah_metal fah_source)

add_library(FAHSaver MODULE FAHSaverView.mm)
target_compile_options(FAHSaver PRIVATE -fobjc-arc)
target_link_libraries(FAHSaver PRIVATE fah_web fah_source "-framework ScreenSaver")
set_target_properties(FAHSaver PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION saver
    PREFIX ""
    OUTPUT_NAME "FAHSaver"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
set(FAH_WEB_ASSETS
    "${PROJECT_SOURCE_DIR}/benchmark/web/saver.html"
    "${PROJECT_SOURCE_DIR}/benchmark/web/saver.css"
    "${PROJECT_SOURCE_DIR}/benchmark/web/capture.js"
    "${PROJECT_SOURCE_DIR}/benchmark/web/scene.js"
    "${PROJECT_SOURCE_DIR}/benchmark/web/gpu-timer.js"
    "${PROJECT_SOURCE_DIR}/benchmark/web/renderer.js"
    "${PROJECT_SOURCE_DIR}/benchmark/web/saver.js")
set_property(TARGET FAHSaver APPEND PROPERTY LINK_DEPENDS ${FAH_WEB_ASSETS})
add_custom_command(TARGET FAHSaver POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_DIR:FAHSaver>/Contents/Resources"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_DIR:FAHSaver>/Contents/Resources/web"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${FAH_WEB_ASSETS}
        "$<TARGET_BUNDLE_DIR:FAHSaver>/Contents/Resources/web"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/third_party/nlohmann/LICENSE.MIT"
        "$<TARGET_BUNDLE_DIR:FAHSaver>/Contents/Resources/nlohmann-json-LICENSE.MIT")

add_executable(fah_macos_saver_smoke SaverSmoke.mm)
target_compile_options(fah_macos_saver_smoke PRIVATE -fobjc-arc)
target_link_libraries(fah_macos_saver_smoke PRIVATE fah_core "-framework AppKit" "-framework WebKit" "-framework QuartzCore" "-framework ScreenSaver")

if(BUILD_TESTING)
    add_executable(fah_macos_web_renderer_tests WebRendererTests.mm)
    target_compile_options(fah_macos_web_renderer_tests PRIVATE -fobjc-arc)
    target_link_libraries(fah_macos_web_renderer_tests PRIVATE fah_web)
    add_test(NAME mac_web_renderer COMMAND fah_macos_web_renderer_tests
        "${PROJECT_SOURCE_DIR}/benchmark/web" "${PROJECT_SOURCE_DIR}/tests/fixtures/synthetic-small.json")
    set_tests_properties(mac_web_renderer PROPERTIES TIMEOUT 60)
    add_executable(fah_macos_session_lifecycle_tests SessionLifecycleTests.mm)
    target_compile_options(fah_macos_session_lifecycle_tests PRIVATE -fobjc-arc)
    target_link_libraries(fah_macos_session_lifecycle_tests PRIVATE fah_core
        "-framework AppKit" "-framework ScreenSaver" "-framework WebKit")
    add_test(NAME mac_session_lifecycle COMMAND fah_macos_session_lifecycle_tests
        "$<TARGET_BUNDLE_DIR:FAHSaver>" "${PROJECT_SOURCE_DIR}/tests/fixtures/synthetic-small.json")
    set_tests_properties(mac_session_lifecycle PROPERTIES TIMEOUT 20)
    add_executable(fah_macos_presentation_lifecycle_tests PresentationLifecycleTests.mm)
    target_compile_options(fah_macos_presentation_lifecycle_tests PRIVATE -fobjc-arc)
    target_link_libraries(fah_macos_presentation_lifecycle_tests PRIVATE fah_core
        "-framework AppKit" "-framework ScreenSaver" "-framework WebKit" "-framework QuartzCore")
    add_test(NAME mac_presentation_lifecycle COMMAND fah_macos_presentation_lifecycle_tests
        "$<TARGET_BUNDLE_DIR:FAHSaver>" "${PROJECT_SOURCE_DIR}/tests/fixtures/synthetic-small.json")
    set_tests_properties(mac_presentation_lifecycle PROPERTIES TIMEOUT 60)
    add_executable(fah_macos_cache_lifecycle_tests CacheLifecycleTests.mm)
    target_compile_options(fah_macos_cache_lifecycle_tests PRIVATE -fobjc-arc)
    target_link_libraries(fah_macos_cache_lifecycle_tests PRIVATE fah_core
        "-framework AppKit" "-framework ScreenSaver" "-framework WebKit")
    add_test(NAME mac_cache_lifecycle COMMAND fah_macos_cache_lifecycle_tests
        "$<TARGET_BUNDLE_DIR:FAHSaver>" "${PROJECT_SOURCE_DIR}/tests/fixtures/synthetic-small.json")
    set_tests_properties(mac_cache_lifecycle PROPERTIES TIMEOUT 60)
    add_executable(fah_macos_options_lifecycle_tests OptionsLifecycleTests.mm)
    target_compile_options(fah_macos_options_lifecycle_tests PRIVATE -fobjc-arc)
    target_link_libraries(fah_macos_options_lifecycle_tests PRIVATE fah_core
        "-framework AppKit" "-framework ScreenSaver" "-framework WebKit")
    add_test(NAME mac_options_lifecycle COMMAND fah_macos_options_lifecycle_tests
        "$<TARGET_BUNDLE_DIR:FAHSaver>" "${PROJECT_SOURCE_DIR}/tests/fixtures/synthetic-small.json")
    set_tests_properties(mac_options_lifecycle PROPERTIES TIMEOUT 45)
    add_executable(fah_macos_render_options_tests RenderOptionsTests.mm)
    target_compile_options(fah_macos_render_options_tests PRIVATE -fobjc-arc)
    target_link_libraries(fah_macos_render_options_tests PRIVATE fah_metal)
    add_test(NAME mac_render_options COMMAND fah_macos_render_options_tests
        "${PROJECT_SOURCE_DIR}/tests/fixtures/synthetic-small.json")
endif()
