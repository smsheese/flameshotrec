# Optional screen recording module

Status: historical implementation proposal. The corrected implementation uses the existing capture overlay and toolbar, with Save finalizing and Cancel discarding video. See [the current workflow](../screen-recording.md); it supersedes the separate overlay/controller and Stop action described below.

## Intended behavior

1. Select an area using the existing Flameshot selection UI. A Record action appears alongside the right-hand actions shown in the reference image.
2. Record validates settings and output location, obtains capture permission if required, and starts live capture of that rectangle. Replace Record with Stop and show Pause plus an elapsed-time indicator. Lock the rectangle for the session.
3. Replace the frozen screenshot editor with a transparent annotation overlay over the live desktop. Keep controls and selection borders outside the encoded rectangle. Keep the desktop at normal brightness during recording.
4. A drawing remains fully visible while being drawn, then for 3 seconds after completion, then fades over 300 ms. Configure the hold and fade durations in Configuration > Video. Existing selection annotations transfer into the recording overlay and start their timers when recording starts.
5. Pause changes to Resume. Paused time contributes no video frames or elapsed recording time. Stop works while paused and finalizes one playable file.
6. After finalization, show the saved path and actions to open the file or folder. Do not report success until FFmpeg exits successfully and the file exists.

Default scope is silent video, one monitor per recording, and a fixed region. Microphone/system audio, webcam, changing the region mid-recording, and cross-monitor recording are follow-up features. This avoids silently expanding the required synchronization and coordinate work.

## Findings in this repository

- `src/widgets/capture/capturewidget.cpp`: `commitCurrentTool()`, `drawToolsData()`, and `processPixmapWithTool()` render tools onto a static screenshot. Recording that pixmap would record a frozen image.
- `src/tools/capturecontext.h`: screenshot data, drawing context, and selection geometry share one context. Do not turn it into a video frame buffer.
- `src/tools/capturetool.h`, `toolfactory.cpp`, and `src/widgets/capture/capturetoolbutton.cpp`: action registration is compile-time. There is no existing runtime add-on loader.
- `src/config/configwindow.cpp`: General, Interface, Filename Editor, and Shortcuts tabs exist; Video must be added.
- `src/utils/confighandler.{h,cpp}`: settings have explicit accessors, defaults, and recognized-key validation. Video settings need the same registration.
- `src/utils/screengrabber.cpp`: existing screenshot capture can inform display geometry and permission handling, but it is not a continuous video source.

## Packaging and interface

Implement a separately built optional module under `src/recording/`, controlled by `ENABLE_RECORDING`. Initially link it as a separate library target. When disabled, hide recording actions and the Video tab and require no recording dependencies. FFmpeg remains an external executable chosen by the user; do not link libav libraries into Flameshot.

This gives add-on-style separation without introducing a general Qt plugin framework or a public binary interface. A dynamically loaded plugin can follow if independent distribution becomes a requirement; that would also require interface versioning, packaging rules, and compatibility checks.

Expose a small `RecordingSession` interface:

- `probe(ffmpegPath)` returns available encoders, muxers, platform capture support, and diagnostic results asynchronously.
- `start(region, settings, initialAnnotations)` starts a session after validation.
- `pause()`, `resume()`, and `stop()` request state transitions.
- Signals report state changes, elapsed active time, warnings, failure details, and the final output path.

All FFmpeg arguments, capture adapters, timestamp handling, annotation lifetime, and output finalization stay inside the module. Flameshot supplies selection geometry and drawing-tool factories through a small host adapter. The module must not reach into `CaptureWidget` private members or call screenshot export tasks.

Proposed internal layout:

```text
src/recording/
  recordingmodule.{h,cpp}       # host registration and settings widget creation
  recordingsession.{h,cpp}     # lifecycle and ownership
  recordingtypes.h             # settings, region, state, capabilities
  ffmpegprobe.{h,cpp}
  ffmpegencoder.{h,cpp}
  recordingoverlay.{h,cpp}     # transparent live annotation surface
  annotationtimeline.{h,cpp}
  recordingcontrols.{h,cpp}
  videoconfig.{h,cpp}
  capture/                    # platform frame-source adapters
  CMakeLists.txt
src/tools/record/             # thin action requesting a session
```

## Capture, overlays, and encoding

Use a continuous platform frame source feeding a bounded frame queue and an external FFmpeg encoder process. Crop and normalize frames to a fixed physical-pixel size. Encode on a worker thread; never send large buffers synchronously from the GUI thread.

The preferred initial path captures the actual desktop including the transparent annotation overlay. Draw annotations once on screen; do not composite them into those captured frames again. This lets users see live applications beneath their markings without copying the screenshot editor's frozen background. Place controls, status, and border outside the crop; when an edge selection leaves no room, put controls elsewhere on the desktop or hide them and provide tray/keyboard controls. Do not rely on hiding controls for each frame.

Overlay inclusion is a platform acceptance gate, not an assumption: the capture spike must prove that visible annotations enter the stream once, the background remains live, and no recursive preview occurs. Do not display a captured-video preview inside the recorded area. If a platform excludes the overlay, that adapter needs a verified clean source plus explicit annotation composition. Never combine desktop-captured annotations and explicit composition. Report unsupported combinations until verified.

Platform adapters:

- Linux X11 first: prototype FFmpeg `x11grab` as a raw-frame producer, crop at acquisition where supported, then feed the encoder through the session's pause/frame gate.
- Linux Wayland: use the ScreenCast portal and consume the authorized PipeWire stream, then crop to the Flameshot region. Permission selection remains compositor-controlled. A local FFmpeg executable alone does not provide this portal integration. Isolate PipeWire dependencies in this adapter.
- Windows: prototype FFmpeg `gdigrab`; replace the acquisition adapter if performance or overlay behavior requires it.
- macOS: prototype FFmpeg `avfoundation` screen input and crop, with screen-recording permission handling. Treat modern native capture as a later adapter if needed.

FFmpeg documents the desktop devices above; device availability depends on the selected build. Wayland uses a separate portal session and PipeWire stream: [FFmpeg devices](https://ffmpeg.org/ffmpeg-devices.html), [ScreenCast portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html).

For every adapter, explicitly map Qt logical selection coordinates to source pixels, including monitor origin, negative desktop coordinates, scale, and source-stream dimensions. Restrict the first release to one monitor. Pad odd dimensions for encoders that require even sizes rather than silently cropping away selection content. Stop safely if the monitor disappears or changes scale/resolution.

## Pause and lifecycle

States: Idle -> Preparing -> Recording <-> Paused -> Finalizing -> Finished; any active state may fail. Preparing covers capability checks, permissions, and source readiness. Buttons reflect confirmed state, with repeated requests ignored while transitioning.

Keep acquisition running during Pause and discard frames. Send only active frames to FFmpeg's fixed-rate raw-video input, so output timestamps remain contiguous and paused time is omitted. Reset frame pacing on Resume; do not flush paused frames into the encoder. Do not use process suspension as pause. Use a monotonic active-time clock for frame scheduling and the displayed timer.

The annotation timeline also freezes during Pause. Disable new drawing while paused; users can switch to interaction mode to operate the underlying application. Resume continues existing annotations from their remaining lifetime.

Apply bounded backpressure: cap queued frames/bytes, discard stale capture frames under load, and use the latest frame for each output tick. Define and test the overload policy so dropping frames cannot shorten the video unexpectedly or grow memory without limit. Warn on sustained inability to maintain the configured frame rate.

Stop ceases frame production, closes the encoder input cleanly, waits asynchronously for finalization, and then publishes the output. Preserve recoverable temporary output and logs on failure. Use a temporary recording filename with an explicit muxer, followed by rename on success. Forced termination is a timed failure fallback, never the normal Stop path. Handle denied permission, process failure, invalid paths, disk full, application shutdown, and stopping during Preparing/Paused.

## Drawing tools and input

Reuse existing pencil, line, arrow, rectangle, circle, and marker tool implementations through a host adapter. Construct a live drawing context with geometry and style, without a frozen desktop background. Keep recording annotations in their own timeline rather than the screenshot undo stack.

Each annotation owns a drawable snapshot and completion time. Compute opacity from a monotonic timeline; do not mutate stored screenshot pixels. Remove an annotation only after its fade finishes, including hit-test and undo references. Keep the active stroke fully opaque until release; cancel incomplete strokes on shutdown.

Provide an explicit Draw / Interact toggle with a shortcut. Draw intercepts pointer events in the region; Interact makes the overlay click-through so users can scroll, click, and demonstrate applications. Verify switching on each supported window system; do not promise unrestricted global shortcuts on Wayland.

First-release tools are vector drawing tools. Text, pixelation, invert, and other tools that depend on editing source pixels need separate live-video semantics. Disable unsupported tools in recording mode with clear UI feedback. Screenshot tools retain their current behavior.

## Configuration > Video

Persist settings through the existing ConfigHandler validation system. Capture an immutable settings snapshot at session start; apply edits to the next recording.

- Enable recording; FFmpeg executable path with Browse and Test. Blank path searches PATH. Show detected version and capabilities; missing FFmpeg disables Start with an actionable explanation.
- Output directory, filename pattern, and container: initially MP4, MKV, and WebM subject to compatible encoders.
- Codec/encoder: initially H.264 and VP9 when available. Distinguish codec from implementation, such as `libx264` or a hardware encoder. Hardware choices require a real short encode probe, not just appearing in `-encoders`.
- Frame rate: presets 15/24/30/60 and a validated custom value; default 30.
- Rate control: quality-based VBR/CRF where supported, target average bitrate (ABR), and constrained/constant bitrate options only where the selected encoder can honor them. Show relevant quality, bitrate, maximum bitrate, and buffer fields; encoder mappings differ.
- Encoder speed preset and compatible pixel format, with sensible encoder-specific defaults. Proposed default: MP4/H.264, 30 fps, quality mode, if available. Offer an explicit supported alternative if absent.
- Annotation hold: default 3000 ms; fade duration: default 300 ms. Define zero hold as immediate fade and zero fade as immediate removal after hold.
- Include cursor, output folder, and keyboard shortcuts for Stop, Pause/Resume, and Draw/Interact.
- Advanced encoder arguments as a structured argument list, with a read-only generated command preview. Pass arguments directly to QProcess, never through a shell. Reserve inputs, output path, transport, timing, and lifecycle flags for the module; reject conflicting overrides. Full arbitrary capture commands would require a separate custom-backend contract.

Probe with bounded, asynchronous `-version`, `-encoders`, `-muxers`, `-devices`, and selected encoder help. Validate combinations with a short synthetic encode before starting a desktop session; cache by executable identity and selected settings. See [FFmpeg options and argument ordering](https://ffmpeg.org/ffmpeg.html).

## Minimal changes to Flameshot

- Add the optional target and registration call in CMake.
- Append a Record tool type, request, factory entry, icon resources, button ordering, and shortcut without renumbering existing tool IDs. Stop and Pause are session controls rather than additional persisted screenshot tool IDs.
- Add one start-recording handoff in CaptureWidget that supplies the fixed region, display mapping, and copies of compatible annotations. Restore the screenshot editor if preparation fails; do not trigger screenshot export or capture-failed notifications merely because recording takes over. Give the recording session independent lifetime ownership.
- Register Video settings and a Video tab using the existing configuration validation/update pattern.
- Share tool construction/rendering through the host adapter. Avoid broad edits to individual drawing tools; only extract a reusable seam where the live overlay actually needs it.

## Delivery sequence and acceptance gates

1. **Capture feasibility spike:** prove live selected-region capture, transparent annotations, click-through interaction, toolbar exclusion, and physical-pixel mapping on X11. Measure raw-frame transport at 1080p/30 and 1080p/60. Repeat the capture/overlay proof on Wayland before claiming Wayland support. Record outcomes and select an adapter-specific composition path.
2. **Optional module and Video settings:** add compile switch, interface, lifecycle, binary discovery/probing, settings widget, and fake source/process adapters for tests. Build both enabled and disabled configurations.
3. **Silent recording:** implement region handoff, source acquisition, bounded pipeline, encoder, Record/Stop controls, output finalization, and diagnostics. Verify that a moving application records live and screenshot capture still works.
4. **Pause/Resume:** add active-time pacing and confirmed state transitions. A 5-second recording, 5-second pause, and 5-second recording must produce approximately 10 seconds of continuous playback with no paused frames.
5. **Transient drawings:** add live overlay, Draw/Interact, reusable vector tools, independent lifetimes, hold/fade settings, and initial annotation transfer. Verify fades visually and in decoded output frames.
6. **Additional platforms and hardening:** implement portal/PipeWire and other adapters, handle permissions/display changes, validate codec/container combinations and resource usage, document supported platforms and FFmpeg requirements.

Focused automated checks: lifecycle transitions including errors and repeated clicks; FFmpeg argument construction; settings validation; coordinate transforms; deterministic annotation opacity; pause timing; bounded-queue overload behavior; process startup/finalization failures. Use a generated moving test source for encode integration checks and inspect decoded frames/duration, with ffprobe if available. Do not require ffprobe as a runtime dependency.

Manual platform gates: real desktop motion, all six drawing tools fading once, pause/resume repeatedly, stopping while paused, controls absent from output, cursor behavior, odd-sized and edge selections, high-DPI and negative monitor origins, permission denial, disk exhaustion, source loss, and normal screenshots with recording enabled/disabled. Current local build validation also requires resolving the previously observed missing Qt6Svg development dependency.

Complete when the selected rectangle records live desktop motion into one valid file, Stop/Pause/Resume work, supported markings hold for the configured delay and fade in both desktop and video, the user can select their own FFmpeg binary and encoding settings, and disabling the module removes its dependencies and UI.
