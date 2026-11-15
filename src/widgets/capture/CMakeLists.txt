# Required to generate MOC
target_sources(
        flameshot
        PRIVATE buttonhandler.h
        capturebutton.h
        capturetoolbutton.h
        capturewidget.h
        colorpicker.h
        hovereventfilter.h
        overlaymessage.h
        selectionwidget.h
        magnifierwidget.h
        notifierbox.h
        modificationcommand.h)

target_sources(
        flameshot
        PRIVATE buttonhandler.cpp
        capturebutton.cpp
        capturetoolbutton.cpp
        capturewidget.cpp
        colorpicker.cpp
        hovereventfilter.cpp
        overlaymessage.cpp
        notifierbox.cpp
        selectionwidget.cpp
        magnifierwidget.cpp
        modificationcommand.cpp)

if(ENABLE_RECORDING)
  # recordingclipboard.cpp is the clipboard/staging half of "copy a recording",
  # x11livedesktop.cpp feeds live invert/pixelate from the window stack below the
  # overlay, and capturerecording.cpp is the overlay side of the session.
  # x11livedesktop.h carries a Q_OBJECT (LiveDesktop::Sampler), so the header is
  # listed for AUTOMOC as well.
  target_sources(flameshot PRIVATE capturerecording.cpp x11livedesktop.cpp
    recordingclipboard.cpp recordingclipboarddaemon.cpp)
  target_sources(flameshot PRIVATE x11livedesktop.h recordingclipboard.h)
endif()
