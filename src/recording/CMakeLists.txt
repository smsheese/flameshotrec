# `videotransferconfig.cpp` reads the application configuration, so it is
# compiled into the executable: the transfer library must not depend on
# configuration globals (see the comment in that file).
target_sources(flameshot PRIVATE videotransferconfig.cpp)

add_library(flameshot-recording STATIC
  ffmpegdiscovery.cpp
  x11recordingbackend.cpp
  recordingoverlay.cpp
  capturerecordingsession.cpp
  audiocapture.cpp
  audiomuxer.cpp
  x11input.cpp
  videotransfer.cpp
  # Per-tool annotation lifetimes. Also used by the Video tab's tool settings
  # section and by the overlay's annotation timer.
  annotationpolicy.cpp
)
target_include_directories(flameshot-recording PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(flameshot-recording PUBLIC
  Qt${QT_VERSION_MAJOR}::Core
  Qt${QT_VERSION_MAJOR}::Gui
  Qt${QT_VERSION_MAJOR}::Widgets
  # videotransfer.cpp is the backwards-compatible facade over the shared
  # remote-storage transfer implementation.
  flameshot-remote
)

# Wayland capture is optional: it uses the desktop portal and PipeWire. Keep
# the X11 recorder buildable when PipeWire development files are unavailable.
if(UNIX AND NOT APPLE)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3)
    if(PIPEWIRE_FOUND)
      find_package(Qt${QT_VERSION_MAJOR} COMPONENTS DBus QUIET)
      if(TARGET Qt${QT_VERSION_MAJOR}::DBus)
        target_sources(flameshot-recording PRIVATE
          wayland/portalpipesource.cpp
          wayland/waylandrecordingsession.cpp)
        target_include_directories(flameshot-recording PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/wayland)
        target_link_libraries(flameshot-recording PUBLIC Qt${QT_VERSION_MAJOR}::DBus PkgConfig::PIPEWIRE)
        target_compile_definitions(flameshot-recording PUBLIC FLAMESHOT_RECORDING_WAYLAND)
      endif()
    endif()
  endif()
endif()
if(UNIX AND NOT APPLE)
  find_package(X11 REQUIRED)
  target_link_libraries(flameshot-recording PUBLIC X11::X11 X11::Xext)
endif()

option(BUILD_RECORDING_TESTS "Build focused recording module tests" OFF)
if(BUILD_RECORDING_TESTS AND UNIX)
  enable_testing()
  add_executable(recording-backend-test tests/recordingbackendtest.cpp)
  target_link_libraries(recording-backend-test PRIVATE flameshot-recording)
  add_test(NAME recording-backend-test COMMAND recording-backend-test)
  find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Test QUIET)
  if(TARGET Qt${QT_VERSION_MAJOR}::Test)
    add_executable(capture-geometry-test tests/capturegeometrytest.cpp)
    target_include_directories(capture-geometry-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(capture-geometry-test PRIVATE
      Qt${QT_VERSION_MAJOR}::Test
      Qt${QT_VERSION_MAJOR}::Gui)
    add_test(NAME capture-geometry-test COMMAND capture-geometry-test)
    add_executable(capture-recording-session-test tests/capturerecordingsessiontest.cpp)
    target_link_libraries(capture-recording-session-test PRIVATE flameshot-recording Qt${QT_VERSION_MAJOR}::Test)
    add_test(NAME capture-recording-session-test COMMAND capture-recording-session-test)
    add_executable(video-transfer-test tests/videotransfertest.cpp)
    target_link_libraries(video-transfer-test PRIVATE flameshot-recording Qt${QT_VERSION_MAJOR}::Test)
    add_test(NAME video-transfer-test COMMAND video-transfer-test)
    # Per-tool annotation policy: on-disk key format, inheritance and the counter
    # group rule. Uses an in-memory store, so no configuration file, display or
    # recording is involved.
    add_executable(annotation-policy-test tests/annotationpolicytest.cpp)
    # The tests use the application include style ("recording/...", "utils/..."),
    # so src/ has to be on the path exactly as it is for the binary.
    target_include_directories(annotation-policy-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(annotation-policy-test PRIVATE flameshot-recording Qt${QT_VERSION_MAJOR}::Test)
    add_test(NAME annotation-policy-test COMMAND annotation-policy-test)
    add_executable(audio-capture-test tests/audiocapturetest.cpp)
    target_include_directories(audio-capture-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(audio-capture-test PRIVATE flameshot-recording Qt${QT_VERSION_MAJOR}::Test)
    add_test(NAME audio-capture-test COMMAND audio-capture-test)
  endif()
  if(TARGET PkgConfig::PIPEWIRE)
    add_executable(wayland-frame-pack-test tests/waylandframepacktest.cpp)
    target_link_libraries(wayland-frame-pack-test PRIVATE flameshot-recording)
    add_test(NAME wayland-frame-pack-test COMMAND wayland-frame-pack-test)
  endif()
endif()
