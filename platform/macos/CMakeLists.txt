add_library(fah_metal STATIC FAHRenderer.mm)
target_include_directories(fah_metal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(fah_metal PRIVATE -fobjc-arc)
target_link_libraries(fah_metal PUBLIC fah_core
    "-framework AppKit" "-framework Metal" "-framework MetalKit" "-framework QuartzCore")

add_executable(fah_macos_preview Preview.mm)
target_compile_options(fah_macos_preview PRIVATE -fobjc-arc)
target_link_libraries(fah_macos_preview PRIVATE fah_metal fah_source)

add_library(FAHSaver MODULE FAHSaverView.mm)
target_compile_options(FAHSaver PRIVATE -fobjc-arc)
target_link_libraries(FAHSaver PRIVATE fah_metal fah_source "-framework ScreenSaver")
set_target_properties(FAHSaver PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION saver
    PREFIX ""
    OUTPUT_NAME "FAHSaver"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")

add_executable(fah_macos_saver_smoke SaverSmoke.mm)
target_compile_options(fah_macos_saver_smoke PRIVATE -fobjc-arc)
target_link_libraries(fah_macos_saver_smoke PRIVATE fah_core "-framework AppKit" "-framework MetalKit" "-framework ScreenSaver")
