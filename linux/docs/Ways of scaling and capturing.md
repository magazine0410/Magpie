# Ways of scaling and capturing

The Windows version has four capture methods to choose from. On Linux there is one way per situation, and Magpie picks it. This page says what each does, what the program that is scaled notices of it, and what it costs in delay.

## Scaling a window that is open

Works on KDE Plasma 6 (Wayland) and on Sway.

Magpie creates a screen that no monitor shows, of exactly the size of the window, and moves the window there without its title bar. It captures that screen and shows the scaled picture on the monitor: above all windows for fullscreen scaling, or in a window of its own for windowed scaling. When scaling ends the window gets its place, size and title bar back.

- **Keyboard:** the window stays the active window, so keys reach it as always.
- **Pointer:** while the pointer is on the picture, it really is on the hidden screen, at the place of the window that the picture shows, so clicks arrive where they are seen. Magpie draws the cursor into the picture. At the edge of the picture the pointer goes back to the monitor.
- **Frames:** on Plasma they come from KWin's screen cast and stay on the graphics card if Magpie renders on the card KWin uses; otherwise, and on Sway, they are copied through main memory.
- **What the program notices:** it is on another screen for the time, of its own size. A program that asks for its screen's size gets that.

Limits:

- Fullscreen scaling ends when another window on the same monitor becomes active. Windowed scaling does not.
- A window of another program that opens while a window is scaled on Sway is moved to the monitor, which ends fullscreen scaling.
- If Magpie is ended by force while a window is scaled, the window stays on the hidden screen. The next start of Magpie puts it back.

## Starting a program in a nested session

Works on every Wayland desktop. Needs the package `sway`.

The Launch button of a profile with "Launch in a nested session", or `magpie-ctl launch -- PROGRAM`, starts a Sway that shows nothing, starts the program in it, and shows that Sway's screen scaled: covering a monitor, or in a window. The window with the picture has the keyboard and the pointer and passes both on.

- **The program sees** a screen of the size set in the profile (1280x720 unless changed). A window that cannot be resized is scaled at its own size.
- **X11 programs** run through that Sway's Xwayland. That is also how Wine shows its windows, but a Windows program has not been tried in a nested session yet.
- **A program that hides the cursor** (most 3D games) gets the mouse's raw movement, as it would on a real screen.
- The scale shortcuts switch between fullscreen and windowed. Stopping ends the program: it has no desktop to return to.
- The clipboard is not shared with the desktop.

**Allow tearing** (a switch of the profile, or `magpie-ctl launch --tearing`): a nested session that covers a monitor shows each frame at once and does not wait for the monitor's refresh. KDE Plasma does this for the active fullscreen window if "Allow tearing in fullscreen windows" is on in its display settings, which it is by default. It removes most of the delay (see below) and can show a break line where two frames meet.

## Delay

Measured on one machine: KDE Plasma 6.8, an NVIDIA RTX 4070 Ti SUPER, a monitor at 200 Hz (5 ms per refresh), a 640x480 source scaled with Lanczos. The numbers are the time from the moment the program drew a frame to the moment the desktop gave it to the monitor; the monitor's own delay is not in them. Medians over about six seconds.

| | Drawn to shown |
|---|---|
| The window without scaling | 10.7 ms |
| Fullscreen scaling | 12 to 16 ms |
| Windowed scaling | 12.6 ms |
| Scaled on the second graphics card of the machine | 18 ms |
| Nested session | 13 to 15 ms |
| Nested session with tearing allowed | 6.5 ms |

What scaling adds is the wait for the capture and under a millisecond of Magpie's own work with a light effect. The capture takes a frame at the next refresh of the hidden screen, which runs at the refresh rate of the monitor: up to one refresh interval, half of one on average for a game with a frame rate of its own. On a 60 Hz monitor that is up to 17 ms, on a 240 Hz monitor up to 4 ms. The way from Magpie to the monitor is the same as for any window; on this machine it took 4 to 14 ms and varied by 3 ms from one run to the next.

Things that change it:

- **A heavy effect** takes the graphics card longer, and frames are dropped when it takes longer than a refresh. The profiler of the toolbar shows the time of each effect.
- **Another graphics card than the desktop's** (a profile can name the card): frames are copied through main memory, and the picture is copied once more on its way to the monitor.
- **The frame rate limiter** of a profile lowers the load, not the delay.

To measure on another machine: the environment variable `MAGPIE_LATENCY_LOG=FILE` makes Magpie write the times of every frame to a file. `linux/tests/tools/latency_desktop_test.sh` in the source tree runs the comparison above with a test program that numbers its frames.

## Monitors with a scale factor

On a monitor set to more than 100 % the picture is rendered with the monitor's real pixels, so a 4K monitor at 200 % gets a 3840x2160 picture.

The window itself is captured on the hidden screen, which has no scale factor. A Wayland program draws itself there at 100 %. An X11 program on a desktop that lets X11 programs scale themselves (the default of Plasma) is drawn by the desktop at a reduced size there and is captured so; for such programs a nested session gives the full picture.

Monitors with different scale factors do not share one picture: with "multi-monitor usage" set to more than one monitor, those with another factor than the window's monitor are left out.

## Several monitors

The picture is on the monitor the window was on, or on all monitors it touched, or on all monitors, as the profile says. The hidden screen takes the highest refresh rate of the monitors that show the picture.

When a monitor that shows the picture is switched off or unplugged, scaling ends. If it was the monitor the window came from, the window is put on another monitor.

## Not supported

- An X11 session.
- Scaling a window that is open on desktops other than KDE Plasma and Sway (GNOME, Hyprland and others); there the nested session is the way.
- HDR: colours are passed on as 8-bit SDR values.
- Touch input.
