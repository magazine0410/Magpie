# Magpie on Linux

Magpie magnifies a window to the size of the screen, or into a larger window, with a choice of scaling algorithms. This is the Linux version of the Windows program. It runs on Wayland.

The other documents in this directory:

- [Ways of scaling and capturing](Ways%20of%20scaling%20and%20capturing.md): what Magpie does on each desktop, and what it costs in delay
- [FAQ](FAQ.md)
- [Compilation guide](Compilation%20guide.md)

The effects are those of the Windows version; `docs/Built-in effects.md` and `docs/MagpieFX (EN).md` in the repository describe them and how to write one.

## What works where

| Desktop | Scaling a window that is open | Starting a program and scaling it |
|---|---|---|
| KDE Plasma 6 (Wayland) | yes | yes |
| Sway | yes | yes |
| Any other Wayland desktop | no | yes |

Starting a program and scaling it uses a *nested session*: the program runs in a compositor of Magpie's own (a Sway that shows nothing), and Magpie shows its picture scaled. This needs the package `sway` to be installed.

An X11 session is not supported.

## Using it

Start Magpie from the application menu. It has a window with the settings and an icon in the system tray.

- **Scale the window you are working in:** press the shortcut (Alt+Shift+A at first). Press it again to stop. Alt+Shift+Q scales into a window that can be moved and resized.
- **Scaling modes** are lists of effects, applied one after the other. The page "Scaling modes" has them.
- **Profiles** hold settings for one program: the scaling mode, the cursor, cropping, and whether Magpie scales the program's window by itself when it becomes active.
- **Launch in a nested session** is a switch of a profile. With it the Launch button starts the program in a session of its own. On desktops other than KDE Plasma and Sway every program is started that way.

On KDE Plasma the keys of the shortcuts are changed in the system settings under "Shortcuts", which the Home page opens. On Sway they are the ones in `config.json`.

## From the command line

`magpie-ctl` talks to the running program:

```
magpie-ctl scale                 # what the shortcut does
magpie-ctl window                # the same into a window
magpie-ctl stop
magpie-ctl launch -- PROGRAM     # start a program in a nested session
magpie-ctl launch --size 640x480 --tearing -- PROGRAM
magpie-ctl status
```

`magpie-ctl --help` lists everything. Further programs:

- `magpie-engine` is the scaling service without the window, for a desktop where the window is not wanted.
- `magpie-scale` scales one window and ends, with every option on its command line.
- `magpie-scale-image` scales a picture file with an effect; `magpie-fxc` compiles an effect file and reports its errors.

## Files

| What | Where |
|---|---|
| Settings | `~/.config/magpie/v5/config.json` (`$XDG_CONFIG_HOME`); the directory named by `MAGPIE_CONFIG_DIR` replaces `~/.config/magpie` |
| Compiled effects | `~/.cache/magpie` |
| Log | `~/.local/state/magpie/logs/magpie.log` |
| Screenshots | `Screenshots` in the pictures directory, unless the profile names another |
| Effects | `/usr/share/magpie/effects` |

A `config.json` of the Windows version can be used. Profiles made on Windows do not apply, because they recognise programs by Windows paths.

## Differences from the Windows version

- While a window is scaled on KDE Plasma or Sway, it is on a screen that exists only for it and that no monitor shows, without its title bar, and it gets its place back afterwards.
- Fullscreen scaling ends when another window on the same monitor becomes active.
- A nested session cannot be left without ending its program. The scale shortcuts switch it between fullscreen and windowed; stopping ends the program. The clipboard is not shared with the desktop.
- Touch input is not passed on.
- There is no update check; the package manager installs updates.
- The capture method of a profile is not used; there is one way to capture on each desktop.
- Texts that only the Linux version has are English in every language.
