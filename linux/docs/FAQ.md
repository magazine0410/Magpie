# FAQ

### Which desktops does Magpie work on?

KDE Plasma 6 on Wayland and Sway can scale a window that is open. Every other Wayland desktop can scale a program that Magpie starts in a nested session. An X11 session is not supported. See [Ways of scaling and capturing](Ways%20of%20scaling%20and%20capturing.md).

### Nothing happens when I press the shortcut.

- Magpie has to be running; its icon is in the system tray. "Run at startup" is on the Settings page.
- The Home page shows the keys that are bound. "Not available" there means the desktop did not register them.
- On KDE Plasma the keys belong to the desktop: System Settings, Shortcuts, Magpie. The first start asks for permission to register them.
- A window that is maximized or fullscreen is only scaled if "Allow scaling maximized or fullscreen windows" is on (Home page, Advanced).

### While a window is scaled it is gone from the task switcher's preview, or shown on another screen.

That is how scaling works here: the window is on a screen of its own that no monitor shows, and Magpie shows its picture. It comes back when scaling ends. If Magpie was ended by force meanwhile, start it again; it puts the window back.

### Fullscreen scaling stops when I click another window.

It ends when another window on the same monitor becomes active, because the picture covers that monitor. Use windowed scaling (Alt+Shift+Q) to work in other windows beside the scaled one.

### The picture is soft.

Choose another scaling mode: Lanczos and FSR keep edges sharper than Bilinear, and the Anime4K and CuNNy effects are made for drawn pictures. If only an X11 program on a monitor with a scale factor is soft, see "Monitors with a scale factor" in [Ways of scaling and capturing](Ways%20of%20scaling%20and%20capturing.md).

### The pointer moves slower or faster on the picture.

On the picture the pointer moves in the pixels of the window, which the picture shows enlarged. "Adjust cursor speed" of the profile slows the pointer down by that factor so that it feels the same. It does so by changing the desktop's pointer speed while the pointer is on the picture, and restores it afterwards.

### A game only runs fullscreen at a low resolution, or is a Windows game.

Make a profile for it and turn on "Launch in a nested session", with the size the game should see. The Launch button then starts it in a session of its own, where it can be fullscreen at that size, and scales it. For a Windows program under Wine, set the launcher of the profile ("Change the executable file for launching") to the script or command that starts it.

### Can a nested session tear to cut the delay?

Yes: "Allow tearing" in the profile. See [Ways of scaling and capturing](Ways%20of%20scaling%20and%20capturing.md) for what it gains.

### Can I use my settings from Windows?

Copy `config.json` to `~/.config/magpie/v5/`. Scaling modes and general settings are used. Profiles are kept but match no program here, because they name Windows paths; make new ones.

### Is there a Flatpak?

No. Scaling a window needs interfaces of the desktop that a sandbox does not pass on. There is an RPM for Fedora; other distributions can build from source ([Compilation guide](Compilation%20guide.md)).

### Something does not work. What should a bug report contain?

- The log: `~/.local/state/magpie/logs/magpie.log`.
- The desktop and its version, the graphics card and its driver.
- The output of `magpie-viewer --capabilities`, which lists what Magpie found on the desktop.

Bugs of the Linux version go to the repository of the Linux version (the About page has the link), not to the project of the Windows version.
