# Changelog

All notable changes to this repository are recorded here. `flameshotrec` is a
fork of [flameshot-org/flameshot](https://github.com/flameshot-org/flameshot):
it keeps upstream Flameshot's screenshot features and continues upstream's
version numbering. Entries below describe what this fork adds on top of the
upstream release named in the heading. Upstream's own release notes are in
`docs/ReleaseNotes_*.md` and are not repeated here.

## [15.1.1] — 2026-11-15

### Documentation

- Replaced the README preview with examples of the recording workflow, its output,
  and the Video settings. No special upgrade steps are required.

## [15.1.0] — 2026-11-15

First feature release of `flameshotrec`. Based on upstream Flameshot 15.0.0.

### Screen recording (new)

- Record from the normal capture window: choosing the record dot turns the
  selected area live, at the position and size already chosen, with the boundary
  and the existing toolbar still visible. The boundary is drawn outside the
  recorded rectangle, so it never enters the video.
- Record, pause and resume with the same dot. Audio that arrives while paused is
  discarded rather than inserted; the saved file has no gap and no silent
  insertion for the paused interval.
- Finish with Save, or leave with Cancel/Exit and discard. Open With saves the
  file first and then offers installed video applications. Cloud saves locally
  and then uploads; a failed transfer never removes the saved local file.
- Copy puts the finished video **file** on the clipboard (`text/uri-list`, the
  GNOME/KDE copy markers and the plain path), so it can be pasted into a file
  manager, chat window or mail composer.
- Annotations draw over the live desktop, with replayable marks: pencil, line,
  arrow, rectangle, circle, marker, rectangular selection, text, counter and
  invert. Marks hold, then fade, then expire on their own. The move tool returns
  the overlay to interacting with applications, so the recorded monitor and every
  other monitor stay usable while recording.
- Invert and pixelate read the live composite, not the capture-time screenshot,
  so a background that changed after the capture window opened is affected
  correctly. The encoder reads the same composite, so screen and video agree.
- Invert's patch is pinned for the hold and fade. Refreshing it after release is
  not possible over a compositing manager, because the patch is then the topmost
  opaque layer in that rectangle. This is documented behaviour, not a defect.
- Marking hold and fade times are configurable globally and per tool. The circle
  counter can either hold until another tool is used, or age the whole group from
  its first bubble.
- Multi-monitor: the area may start and end on any monitor, including one whose
  origin is left of or above the primary monitor. Recording supports a region on
  one monitor with a common scale factor; `flameshot gui --monitor N` restricts a
  capture to one monitor.
- Defaults: video folder `~/Videos/flameshot`, image folder
  `~/Pictures/flameshot` (an existing explicitly configured image path is kept),
  strftime filename patterns, numeric suffixes on collisions.

### Audio (new)

- System audio is recorded by default; the microphone is off until switched on.
  Each source has its own device selector: "follow the default output/input"
  (PulseAudio or PipeWire) or a pinned source name.
- Audio quality selects the capture rate — 48 kHz (default), 44.1 kHz or 32 kHz —
  and the bitrate selects the encoder rate, 32 to 512 kbit/s (128 default).
- The container decides the audio codec: AAC for MP4/MKV, Opus for WebM. Audio
  shorter than the video is padded with silence rather than shortening the
  recording; a fully paused recording is saved without an audio track and says so.
- A requested source is never dropped silently. Recording refuses to start, with
  the reason shown, when the sound server is unreachable, the source does not
  exist, FFmpeg has no PulseAudio input, or the selected encoder is missing.

### Remote storage (new)

- One remote configuration is shared by screenshots and recordings: FTP, SFTP,
  SSH (streamed over `ssh`) and S3-compatible object storage, with host, port,
  user, SSH private key, `known_hosts`, and remote directory.
- Screenshots and recordings have separate destinations — a remote directory or,
  for S3, an object key prefix — so images and videos do not land in one folder.
- Each media kind uploads either the saved file byte for byte or a converted
  copy: images choose format, quality and compression level; videos choose
  container, encoder, rate control (CRF or average bitrate), quality, bitrate and
  audio handling.
- Secrets are never persisted: the FTP password and S3 secret key are asked for at
  upload time or read from the environment, and are kept out of the configuration
  file, the process argument list and the log. SFTP/SSH verify the host against a
  configured `known_hosts` with strict checking; FTP requires an explicit
  acknowledgement that the password and file travel unencrypted.
- The same transfer is available for ordinary image captures, not just videos.

### AI assistance

- Toolbar AI button carrying the features of
  [flameshot-ai](https://github.com/agarnung/flameshot-ai): Ask, Translate and
  Explain a capture, with an optional dialog to pick the mode and prompt.
- Works against any OpenAI-compatible chat completions endpoint: API URL, bearer
  token and model name are configuration fields. The token is stored in the
  configuration file, so keep that file private.
- Optional: copy the text result to the clipboard when the chat window closes.

### Configuration

- New **Video** tab: FFmpeg executable (blank auto-detects from `PATH` and common
  install locations), video folder, filename pattern, frame rate,
  container/encoder, rate control and quality or bitrate, audio sources and
  quality, annotation hold/fade times (global and per tool) and the counter mode.
  Incompatible encoder/container combinations are disabled.
- New **Remote Storage** tab: connection, protocol, SSH key/`known_hosts`,
  S3 endpoint/region/bucket/prefix and the two media profiles.
- The FTP/SFTP options that used to live in the Video tab moved to Remote Storage.

### Fixes

- A finished recording is published atomically. A half-written file is never left
  under the final name, and a failed publication keeps the local file.
- The encoder status line is no longer burned into the video. Status text is drawn
  in the overlay only, never in the layer the encoder reads.
- Copy staging now fails closed and only ever touches Flameshot's own folder: it
  is created with owner-only permissions and a marker. An existing unmarked
  directory of the same name is not adopted, marked, swept, or deleted; a fresh
  private folder is used instead. Staged recordings that the clipboard no longer
  offers are removed after 24 hours.
- The annotation timer no longer repaints during a hold. It sleeps until the hold
  ends and then repaints only the fading annotations' rectangles, instead of the
  whole desktop-sized overlay.
- Expired annotations are freed, and undo snapshots no longer reference
  annotations that were already released.
- Recording refuses to start with a message instead of starting a capture that
  silently produces a file without the requested audio, when `pactl`, the device,
  FFmpeg's PulseAudio input or the required encoder is missing.
- Open With accepts a saved video path directly, so a large recording is not
  copied into a temporary file first.

### Packaging, build and installation

- `ENABLE_RECORDING` is a CMake option, on by default on Linux and off elsewhere;
  `-DENABLE_RECORDING=OFF` builds without the recorder. Builds need the X11 and
  Xext development headers for the recording path.
- CPack produces a Debian `flameshot` package containing only the runtime
  component (`/usr/bin/flameshot`, resources, desktop entry, man page, icons),
  with the shared libraries resolved by `shlibdeps`, `ffmpeg`, `curl` and
  `openssh-client` as dependencies and `pulseaudio-utils` as a recommendation.
- **The menu entry depends on the install prefix.** `USE_LAUNCHER_ABSOLUTE_PATH`
  bakes the configure-time `CMAKE_INSTALL_FULL_BINDIR` into the desktop file. A
  package built from a tree configured for `/usr/local` but installed under
  `/usr` gets an `Exec` line that does not exist, and GLib then drops the entry
  entirely — the application is not merely unlaunchable, it never appears in the
  menu. Build packages with `-DCMAKE_INSTALL_PREFIX=/usr`, and verify the packaged
  desktop file resolves (for example with `Gio.DesktopAppInfo`) rather than
  trusting `desktop-file-validate`.
- Debian package `15.1.0-1` (the development packages used the `-Nrecording`
  suffix; this release uses the plain revision).

### Upgrading

1. Quit the running Flameshot tray process, then install the new build and start
   Flameshot again. A process left from the old binary keeps showing the old
   settings and tools until it is restarted.
2. Optional dependencies are only needed for the features that use them:
   FFmpeg (with `x11grab` and PulseAudio input) for recording, `pactl` from
   `pulseaudio-utils` to follow default audio devices, the OpenSSH client for
   SFTP/SSH, `curl` for FTP, and an X11 compositing manager for the X11 recording
   path.
3. Recording is X11-only for now. The Wayland portal/PipeWire adapter is built
   when the development files are present, but the Record action is disabled on
   Wayland sessions until region capture and overlay placement are verified on a
   real compositor.

### Known limitations in this release

- Recording requires a compositing window manager and FFmpeg's `x11grab` input,
  and covers a region on one monitor.
- The inverted patch cannot follow changes after the stroke is released.
- Recording is not built for Windows or macOS.
- The AI assistant supports OpenAI-compatible chat completions endpoints with
  image input; it is not a client for every OpenAI product or API shape.
