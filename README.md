# C1 Tune for Omarchy

A small, fast tool for the **Opal C1** webcam on [Omarchy](https://omarchy.org) (Arch +
Hyprland): real hardware controls, applied by the camera itself, and nothing else. A port of
[C1 Tune for macOS](https://github.com/ryankirkman/c1-tune).

- **Fast.** MEASURED_STARTUP
- **Real controls.** Exposure, focus, white balance, color, and zoom up to 2× without
  upscaling. The camera shows up as a plain UVC webcam to every app.
- **Lightweight.** The app that stays running is one ~380 KB binary linking only libc and
  libstdc++. No virtual camera, no PipeWire nodes, no services, no polling.
- **Safe.** Nothing is ever written to the camera's flash; unplugging it restores factory
  behaviour. If the app quits, the camera drops back to its factory webcam mode within a
  couple of seconds.

Not affiliated with or endorsed by Opal Camera.

## Install

```bash
git clone https://github.com/ryankirkman/c1-tune-omarchy.git && cd c1-tune-omarchy
make install     # builds the Arch package and installs it with pacman (asks for sudo)
```

The package installs `c1-tune`, its control panel, a launcher entry, and the udev rule that
lets you (not just root) talk to the camera. Then open **C1 Tune** from the launcher
(`Super + Space`) and tick **Open at login**. Settings are saved and re-applied whenever the
camera connects or an app starts using it.

The first start downloads the camera's firmware from Luxonis (8 MB, once, SHA-256 pinned).

To run from the build directory instead, install just the udev rule:

```bash
make && make udev && build/c1-tune --panel
```

### On the bar, or on a key

A bar button for the Omarchy shell ships with the package:

```bash
mkdir -p ~/.config/omarchy/plugins
cp -r /usr/share/c1-tune/bar-widget ~/.config/omarchy/plugins/com.ryankirkman.c1-tune
omarchy plugin enable com.ryankirkman.c1-tune
```

Or bind a key in `~/.config/hypr/bindings.lua` (pick one that's free on your setup):

```lua
o.bind("SUPER + ALT + C", "C1 Tune", "c1-tune --panel")
```

Either one opens the panel, and closes it if it's already open.

## Controls

| | |
|---|---|
| Exposure | Auto with bias (−9…+9), or manual shutter (0.1–33 ms) and ISO (100–1600) |
| Focus | Continuous autofocus, or manual lens position |
| White balance | Auto, or manual 1000–12000 (the firmware's scale, not true Kelvin) |
| Image | Brightness, contrast, saturation, sharpness, denoise, color denoise |
| Anti-flicker | Auto / 50 Hz / 60 Hz / off |
| Zoom | 1×–2× in 20 steps, never upscaled |
| Framing | Moves the zoomed picture left–right and up–down (snaps to center) |
| Flip image | Turns off the default 180° rotation, for a camera mounted upside down |
| Microphone | The camera's built-in mic, off by default |
| Preview | Optional live view in the panel (needs `qt6-multimedia`) |

Most changes apply instantly. Zoom, framing, flip, the microphone, the tuning file, and
returning sharpness/denoise to default restart the camera (about 5 seconds, once per burst
of changes).

In the panel: `Esc` or `Q` closes it, `P` toggles the preview, `R` retries after a failure.

**Scripting:** every setting can be set from the command line, with the same keys as the
macOS app:

```bash
c1-tune --set zoom=125 frameY=60 saturation=1
c1-tune --status          # state and settings as JSON
```

`autoExposure exposureUs iso exposureCompensation autoFocus lensPosition autoWhiteBalance
whiteBalanceK brightness contrast saturation sharpness noiseReduction colorNoiseReduction
antiFlicker zoom frameX frameY flipped microphone tuningFile`

`default` clears sharpness or a denoise value back to the tuning's own. `c1-tune --help`
lists the other commands (`--reset`, `--retry`, `--quit`).

## Tuning file

On macOS, C1 Tune uses the image tuning that ships inside Opal Composer. Opal Composer
doesn't exist on Linux and its files can't be redistributed, so here the camera runs on
**the firmware's own tuning** unless you bring Opal's over from a Mac:

```bash
# on the Mac
scp "/Applications/Opal Composer.app/Contents/XPCServices/OpalCameraDeviceService.xpc/Contents/Resources/Opal_v0.12.bin" you@linux:~/.config/c1-tune/
```

A file with that name in `~/.config/c1-tune/` is picked up on the next camera start
(`Opal582_v2.bin` for the IMX582 revision). Or point at a file anywhere:

```bash
c1-tune --set tuningFile=/path/to/Opal_v0.12.bin     # tuningFile=default to stop using it
```

The defaults (bias −1, saturation +1, sharpness 3) were chosen on top of Opal's tuning, so
expect to adjust them without it.

## How it works

The [macOS README](https://github.com/ryankirkman/c1-tune#how-it-works) explains the camera
side: reboot to the bootloader, drop to the USB boot ROM, upload DepthAI firmware to RAM,
start a pipeline over RPC, and send every setting as a `CameraControl` message. This port
sends the camera exactly the same bytes in the same order; `make test` checks the encoders
against the same reference fixtures, byte for byte.

What's different is the Linux side:

| macOS | Here |
|---|---|
| IOKit | usbfs (`/dev/bus/usb`) ioctls, no libusb |
| Polling for the camera every 1.5 s | udev's netlink broadcasts: the app sleeps until a device arrives or leaves |
| CoreMediaIO "is running somewhere" | inotify on the camera's `/dev/video*` node: an app opening it triggers the re-send |
| Menu bar popover (AppKit) | A separate Qt Quick panel that follows the Omarchy theme, talking to the app over a Unix socket |
| Opal Composer's firmware and tuning | Luxonis's firmware download; optional tuning file |
| Login item | An XDG autostart entry (`~/.config/autostart/c1-tune.desktop`) |
| `disable-opal.sh` | Not needed: nothing else on Linux claims the camera |

The app (`c1-tune`) and the panel (`c1-tune-panel`) are separate programs on purpose. The
app is what stays running, so it links no toolkit and does nothing until the kernel wakes
it; the only thing it does on a clock is feed the camera's watchdog (one 4-byte USB write
every half second, which the camera requires). The panel exists only while it's open.

| File | |
|---|---|
| `src/depthai.*` | libnop/msgpack encoders, boot image + descriptor patch, pipeline schema, RPC |
| `src/settings.*` | Saved settings → camera commands, zoom/framing |
| `src/usb.*` | usbfs access to each boot stage, udev event monitor |
| `src/xlink.*` | XLink link protocol (84-byte headers, streams, watchdog keepalive) |
| `src/session.*` | Boot/attach state machine and control delivery |
| `src/firmware.*` | Firmware download and pinning, tuning file lookup |
| `src/main.cpp` | The app's event loop, its socket, and the command line |
| `panel/` | The control panel |
| `bar-widget/` | A button for the Omarchy bar |
| `pkgbuild/` | Arch package, udev rule, launcher entry, icon |

Files: settings in `~/.config/c1-tune/settings.json`, firmware in `~/.local/share/c1-tune/`.

## Make targets

| | |
|---|---|
| `make` | Build `build/c1-tune` and `build/c1-tune-panel` |
| `make test` | Protocol tests (no camera needed; `C1TUNE_TEST_DOWNLOAD=1` also checks the firmware download) |
| `make install` | Build the Arch package and install it |
| `make udev` | Install only the udev rule |

Uninstall with `sudo pacman -R c1-tune`; settings and the cached firmware are the two
directories above.
