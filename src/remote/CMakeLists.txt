# Remote storage and export. Shared by screenshots and recordings, so the core
# lives in its own library and is part of the main binary even when
# ENABLE_RECORDING is off.

add_library(flameshot-remote STATIC
  s3signer.cpp
  s3signer.h
  remotestorage.cpp
  remotestorage.h
  remotestoragekeys.h
  mediaexporter.cpp
  mediaexporter.h
  remoteuploader.cpp
  remoteuploader.h
)
# Keep the Qt MOC case in mind: remotestorage.h/mediaexporter.h have no Q_OBJECT,
# remoteuploader.h does, so it is listed above for AUTOMOC.
target_include_directories(flameshot-remote PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(flameshot-remote PUBLIC
  Qt${QT_VERSION_MAJOR}::Core
  Qt${QT_VERSION_MAJOR}::Gui
  Qt${QT_VERSION_MAJOR}::Network
)

# Configuration widget and the progress/error dialog need the application
# configuration, so they are compiled straight into the executable.
target_sources(flameshot
  PRIVATE remotestoragestore.cpp
          remotestoragestore.h
          remotestorageconf.cpp
          remotestorageconf.h
          remoteuploaddialog.cpp
          remoteuploaddialog.h
)
# Plain signature: src/CMakeLists.txt links flameshot-recording the same way.
target_link_libraries(flameshot flameshot-remote)

option(BUILD_REMOTE_STORAGE_TESTS "Build focused remote storage tests" OFF)
if(BUILD_REMOTE_STORAGE_TESTS)
  enable_testing()
  find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Test QUIET)
  if(TARGET Qt${QT_VERSION_MAJOR}::Test)
    # Model, signing, quoting and conversion checks: no service required.
    add_executable(remote-storage-test tests/remotestoragetest.cpp)
    # The tests use the application include style ("remote/..."), so src/ is on
    # the path as it is for the rest of the binary.
    target_include_directories(remote-storage-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(remote-storage-test PRIVATE flameshot-remote
      Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Widgets)
    add_test(NAME remote-storage-test COMMAND remote-storage-test)

    # End-to-end transfers against loopback fixtures (tests/run.sh starts them)
    # and against PATH shims for ssh/sftp.
    add_executable(remote-upload-test tests/remoteuploadtest.cpp)
    target_include_directories(remote-upload-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(remote-upload-test PRIVATE flameshot-remote
      Qt${QT_VERSION_MAJOR}::Test)
    add_test(NAME remote-upload-test COMMAND remote-upload-test)
  endif()
endif()
