# Screen recording

Select an area directly in the normal Flameshot capture window, then choose the
record dot. The selected area becomes live and its boundary and existing toolbar
remain visible. The boundary is drawn just outside the recorded area, so neither
it nor the selection handles appear in the video. The dot changes to Pause;
choosing it again resumes recording.
The existing pencil, line, arrow, rectangle, circle, marker, rectangular
selection, text, counter and invert tools draw over the live desktop. Completed
marks stay for three seconds by default, measured from the moment the stroke is
released, then fade. The move tool switches back to interacting with
applications. While interacting, the selection overlay does not take pointer
events, so applications on the recorded monitor and on every other monitor keep
working normally; only the toolbar accepts clicks. Selecting a drawing tool
makes the recorded area accept drawing again. The selection stays fixed until
recording ends.

## Annotations, the annotation timer and Invert

Annotations are painted by the overlay itself, so the encoder reads exactly what
the user sees: one screen read per invert drag, and no per-frame screen reads at
all.

The annotation timer is not a repaint clock. While a mark is inside its hold
nothing on the overlay changes, so the timer sleeps until the hold ends and then
runs at 60 Hz only for the fade. The fade repaints the fading annotations'
rectangles (dirty-region updates), never the whole desktop-sized overlay. The
integration harness measures this: 0 paint events during one second of a hold,
8 during a 300 ms fade.

Invert is the one annotation that needs desktop pixels, and it reads them from
the live composite rather than from Flameshot's capture-time screenshot:

- While the box is dragged, the recorded region is read once, before the
  annotation paints anything there, so the sample cannot contain the overlay's
  own pixels. The patch is then built from that sample, which is why a background
  that changed after the capture window opened is inverted correctly.
- After release, the inverted patch is pinned for the hold and the fade. It
  cannot be refreshed: with a compositing manager the patch is the topmost
  opaque layer in that rectangle, so any later read returns the patch itself
  (inverting it again is a no-op that also hides real changes), and hiding the
  patch to read underneath would flicker on screen and flash an uninverted area
  into the recording. This is a limitation of compositor-side blending (only
  over-compositing is available to a client), not a bug to be fixed by grabbing
  more often.
- The recorded file gets the same pixels as the screen: the patch is part of the
  composite that the encoder reads, so screen and video never disagree.

The integration harness runs on a real compositing manager (see
`tests/integration/run.sh`), so these are observed pixels and not a mocked guard:
an invert box over a window that changed from red to blue after the capture
window opened is encoded 100% as the inverse of the new colour, and its
capture-time inverse appears in no frame. Where no compositing manager is
available the harness says so and skips the live-pixel comparison instead of
pretending it passed.

- Save finishes the recording and publishes it in the video save folder.
- Cancel/Exit discards the in-progress recording and leaves the capture window.
- Open With saves the video first, then offers installed video applications.
- Cloud saves locally first, then transfers through configured FTP or SFTP.
  Transfer failure never removes the local saved capture.

Configuration > Video contains the FFmpeg executable, save folder and filename
pattern, codec, container, frame rate, bitrate/quality mode, audio sources and
quality, annotation hold/fade times, and FTP/SFTP settings. Incompatible
encoder/container choices are disabled.
The default video folder is `~/Videos/flameshot`. Image captures default to
`~/Pictures/flameshot`; an existing explicitly configured image path is retained.
The Filename Editor controls image names, and the Video tab controls video names.
Both patterns accept strftime fields, for example `%F_%H-%M-%S`. Name collisions
receive a numeric suffix.

## Audio

Recordings capture system audio by default; the microphone is off until it is
switched on. Configuration > Video lists the available PulseAudio or PipeWire
devices and offers one selector per source:

- System audio follows the monitor source of the current default output device.
  Choosing a specific monitor in the list pins that device instead.
- The microphone follows the default input device, or the selected input source.
- Audio quality selects the capture rate: 48 kHz (High, default), 44.1 kHz
  (Medium) or 32 kHz (Low). Audio is captured as stereo.
- Audio bitrate selects the encoder bitrate, 32 to 512 kbit/s (128 by default).
- The container decides the codec: AAC for MP4 and MKV, Opus for WebM. The
  matching encoder must exist in the selected FFmpeg build.

Pause removes audio from the recording exactly like it removes video: audio that
arrives while paused is discarded, and the saved file contains no gap or silent
insertion for the paused interval. If the captured audio is shorter than the
video, the muxer pads it with silence to the video length instead of shortening
the recording. When the whole recording is paused, the file is saved without an
audio track and Flameshot reports that instead of silently dropping the request.

A requested source is never dropped without a message. Recording refuses to
start — with the reason shown — when the sound server cannot be reached, when the
selected source does not exist, when the FFmpeg build has no PulseAudio input,
or when it lacks the encoder the selected container needs.

Following the default output and default input requires `pactl`, which ships with
PulseAudio (`pulseaudio-utils`) and also drives PipeWire's PulseAudio server.
Packages recommend it rather than requiring it, because a fixed source name in
the configuration needs no device discovery. Audio capture itself needs an FFmpeg
build with PulseAudio input support.

The sound sources are captured as raw PCM until the recording is muxed, which
needs about 190 kB per second and per source (roughly 690 MB per hour). That
workspace lives in the recording's own temporary folder and is deleted as soon as
the finished file is published.

## Copying a recording

Copy puts the finished video **file** on the clipboard, not a screenshot, so it
can be pasted into a file manager, a chat window or a mail composer; the paste
target receives the URI in the flavours those applications read (`text/uri-list`,
the GNOME/KDE copy markers and the plain path). The file is staged in Flameshot's
own private folder under the system temporary location, never in your video
folder, and it stays there while the clipboard still offers it so a late paste
works. Staged recordings that nothing offers any more are removed after 24 hours.

That staging folder is created by Flameshot and marked as Flameshot's own, with
owner-only permissions. If a directory with the same name already exists and does
not carry that marker — for example a folder you created yourself, or one left by
a version that did not mark it — Flameshot leaves it completely alone: it is not
adopted, not marked, not swept, and nothing inside it is deleted. A fresh private
folder is used for the recording instead, so Copy keeps working. You can delete
such an old folder yourself at any time; Flameshot will never do it for you.

For FTP, configure the server, username, port, and remote directory; the password
is requested at upload time and is not saved. SFTP uses the selected private key
and a trusted `known_hosts` file with strict verification. Load a passphrase-
protected private key into your SSH agent before uploading. The destination
directory must already exist. The same cloud transfer can save and upload image
captures. The runtime package requires FFmpeg, curl, and the OpenSSH client.

Flameshot searches PATH and common installation directories for FFmpeg. To use
a different build, choose its executable in the Video settings. That FFmpeg
build must provide the selected capture device and encoder.

Ordinary screen capture covers every monitor at once: Print Screen, the tray
icon and `flameshot gui` shade the whole desktop, so a region can be started or
finished on any monitor, including a monitor whose origin is left of or above
the primary one. Use `flameshot gui --monitor N` (or Capture Active Monitor) to
restrict a capture to one monitor, and `flameshot screen` to capture a single
monitor without any interaction.

Recording is enabled by default on Linux builds and can be omitted with
`-DENABLE_RECORDING=OFF`. The X11 recording path requires a compositing window
manager and FFmpeg's `x11grab` input. Recording currently supports a region on
one monitor, with a common scale factor across the desktop. Leave space outside
the selected area for the toolbar so it does not appear in the video; the
selection boundary is excluded automatically.

After upgrading an installed package, quit the old tray process and start
Flameshot again. A process left running from the old binary continues to show
the old settings and tools until restarted.

Wayland: a portal/PipeWire capture adapter is built when PipeWire and Qt DBus
development files are present, but the Record action is deliberately disabled
on Wayland sessions until region capture and overlay placement have been
verified on a real compositor. Use an X11 session to record for now.
