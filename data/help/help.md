This is the text of FujiNet Go Apple II's built-in Help (Help ▸ FujiNet Go Apple
II Help, or Ctrl+Shift+H; ⌘? on a Mac). The app shows each part only to the
frontend it is about; here you see them all. Every topic below is one page in
the app.

# Getting started

FujiNet Go Apple II is an Apple II with a FujiNet card in slot 7. The machine is
AppleWin's: the 6502, the video, the sound, and the cards in the slots. The
FujiNet runs inside the app, as a SmartPort device on the slot 7 card, so the
Apple //e starts straight into FujiNet's CONFIG, where you can pick a disk
image to boot.

1. **Start the app.** The machine boots from slot 7 into CONFIG. The system ROMs
   are part of the app. Type on your keyboard: it is the Apple II's keyboard.
   See [Controllers and keys](#controllers-and-keys).
2. **Pick a disk in CONFIG.** Browse hosts and the SD folder, and boot an image.
   See [FujiNet](#fujinet).
3. **Or bring your own files.** **Import Disk Image…** copies a disk image into
   FujiNet's SD folder, and **Import System ROMs…** adds ROM files AppleWin
   looks for by name. See [Disks and ROMs](#disks-and-roms).
4. **Change the machine.** [Preferences](#preferences) picks the model, the video
   mode, the cards in the slots and the picture.

## This Help

The list on the left has a topic for each part of the app; the search box
above it finds every section that has all the words you type. A **Help**
button in Preferences, the Debugger and the FujiNet console log opens the topic
for what it shows, and <kbd>Ctrl+Shift+H</kbd> does the same for the window you
are in.

<!-- only: gnome kde windows -->

In Preferences, the Debugger and the console log, <kbd>F1</kbd> does too. In the
main window <kbd>F1</kbd> goes to the machine.

<!-- end -->

The same text is on GitHub, as
[data/help/help.md](https://github.com/FujiNetWIFI/fujinet-go-apple2-desktop/blob/main/data/help/help.md).

<!-- only: macos -->

On a Mac, the Help menu's search field also finds Help topics.

<!-- end -->

<!-- context: main -->
# The window

The machine's picture fills the window. The menus have:

- **Reset** and **Power Cycle**: see [Reset and power](#reset-and-power).
- **Import Disk Image…** and **Import System ROMs…**: see
  [Disks and ROMs](#disks-and-roms).
- **FujiNet Configuration…** and **FujiNet Console Log…**: see
  [FujiNet](#fujinet).
- **Debugger** (<kbd>F12</kbd>), **Preferences**, **Help** and **About**.

<!-- only: gnome -->

On GNOME, everything is in the main menu (the button at the right of the
header bar; <kbd>F10</kbd> opens it).

<!-- end -->

<!-- only: kde -->

The menu bar has Machine, Media, FujiNet, View, Settings and Help. Preferences is
in the Settings menu.

<!-- end -->

<!-- only: windows -->

The menu bar has Machine, Media, FujiNet, View and Help. The Machine menu has the
Model, Video and Slot menus, and the FujiNet menu has **Connection…**; there is
no separate Preferences window: see
[Preferences on Windows](#preferences-on-windows). The View menu has the Aspect
Ratio and Scanlines choices and Fullscreen (<kbd>F11</kbd>).

<!-- end -->

<!-- only: macos -->

On a Mac, Settings and Quit are in the app menu, and the Machine, Media, FujiNet
and View menus have the rest.

<!-- end -->

## Reset and power

- **Reset** is the Apple II's reset key.
- **Power Cycle** is the power switch: the machine starts again from nothing, and
  boots the card in slot 7 into CONFIG.

## The picture

The picture can be shown television-shaped, square or in whole steps, and with
scanlines. Both are in [Preferences](#preferences). <kbd>F11</kbd> toggles full
screen where the app has it.

# Controllers and keys

The keyboard is the Apple II's keyboard: a key you press types on the Apple II,
and both the press and the release go to the machine.

## The keyboard

The two Alt keys are the Apple II's Open Apple (left) and Closed Apple (right),
which games and the boot ROM use, so they go to the machine rather than the
window manager. <kbd>Ctrl</kbd> and <kbd>Shift</kbd> are the Apple II's. The app
keeps only these keys for itself:

| Shortcut | Does |
|---|---|
| <kbd>Ctrl+Shift+H</kbd> | Help |
| <kbd>F11</kbd> | Fullscreen |
| <kbd>F12</kbd> | Debugger |

<!-- only: gnome -->

<kbd>F10</kbd> opens the main menu.

<!-- end -->

<!-- only: linux -->

Key combinations with the Super (Windows) key are the desktop's.

<!-- end -->

## Paddles and gamepads

Gamepads are picked up as they are plugged in. A pad's stick drives the Apple II's
two analog paddles, PDL0 and PDL1, with a dead zone that is taken off so the
paddle still reaches its full travel, and its buttons are the paddle buttons.
The first pad drives them unless another is chosen.

# Machines

Preferences pick what the app emulates, and a change restarts the machine:

- **Model**: Enhanced Apple //e (the default), Apple \]\[ (Original), Apple \]\[+,
  Apple \]\[ J-Plus, Apple //e, Pravets 82, Pravets 8M, Pravets 8A, Base64A or
  TK3000 //e.
- **Video**: how the picture is drawn: Color (Composite Monitor) (the default),
  Color (Composite Idealized), Color TV, B&W TV, Monochrome (Amber), Monochrome
  (Green), Monochrome (White) or Color (RGB Card/Monitor). The composite monitor
  is the most faithful to an Apple II and gives the right hi-res colours.
- **Slots** hold the cards:

| Slot | Choices |
|---|---|
| Slot 3 | Empty or Video HD |
| Slot 4 | Empty, Mockingboard, Mouse or Phasor |
| Slot 5 | Empty, CP/M, Mockingboard, Phasor, SAM/DAC or FujiNet |
| Slot 6 | Disk II or Empty |
| Slot 7 | Empty, Hard Disk or FujiNet |

FujiNet lives in slot 7, the bootable SmartPort slot the //e scans before the Disk
\]\[ in slot 6. A Video HD card in slot 3 changes the picture's size.

# Disks and ROMs

The system ROMs are part of the app. **Import System ROMs…** adds more: AppleWin
looks ROMs up by exact file name (`Apple2e_Enhanced.rom`, `DISK2.rom`, and so on),
so each file you pick keeps its name in the app's `roms` folder.

## Import Disk Image

**Import Disk Image…** copies an image into FujiNet's SD folder, so CONFIG can
list and boot it. The image is a `.dsk`, `.do`, `.po`, `.2mg`, `.nib`, `.woz` or
`.hdv` file. Anything else is refused.

# FujiNet

The FujiNet is FujiNet's own firmware, built as a shared library and run inside
the app. It joins the emulator over loopback TCP, as a SmartPort-over-SLIP device
(port 1985), in the card in slot 7. The Apple //e autostart ROM scans slot 7
before the Disk \]\[ in slot 6, so the machine boots straight into CONFIG.

## CONFIG

CONFIG is FujiNet's program for choosing what to boot: the SD folder and servers
on the network. Pick an image and the machine boots it.

## The FujiNet menu

- **FujiNet Configuration…** shows FujiNet's own web page, which mounts disks,
  joins networks and sets up hosts. It is on port 64001 on this computer only.
- **FujiNet Console Log…** shows what FujiNet prints. See
  [The console log](#the-console-log).

<!-- context: fujilog -->
## The console log

The FujiNet Console Log window shows FujiNet's own messages as they come,
following the end while you are at the bottom.

## An external FujiNet

**Use external FujiNet** skips the built-in FujiNet: the app listens on the
**Listen address:port** you give, for example `0.0.0.0:1985`, and waits for
another instance to connect to the slot 7 card. Point that instance's
SmartPort-over-SLIP client at this address. `0.0.0.0` takes connections from the
network, and `127.0.0.1` from this computer only. Leave the field empty for
`127.0.0.1:1985`. FujiNet Configuration and the console log belong to the
built-in FujiNet, so they say the FujiNet is external.

## Behind the scenes

- FujiNet's SmartPort link is on port 1985 and its web UI on 64001, on this
  computer only. A stray standalone `fujinet-pc-apple2` holding 1985 makes the
  link misbehave; stop it, or point the app at it.
- FujiNet's settings and its SD folder are in the app's `fujinet` folder.

# Preferences

Preferences set up the machine, the FujiNet link and the picture. The Machine,
Slots and FujiNet settings apply by restarting the session when Preferences
closes; the Display settings apply at once. The settings are one file, shared by
every frontend of the app on the computer.

<!-- only: macos -->

On a Mac this window is called **Settings** (⌘,).

<!-- end -->

<!-- context: prefs/Machine -->
## Machine settings

- **Model**: which Apple II to emulate (setting `machine`).
- **Video**: how the picture is drawn (setting `video_mode`).

## Slot settings

- **Slot 3**, **Slot 4**, **Slot 5**, **Slot 6** and **Slot 7**: the card in each
  slot (settings `slot3`, `slot4`, `slot5`, `slot6` and `slot7`). See [Machines](#machines).

<!-- context: prefs/FujiNet -->
## FujiNet settings

- **Use external FujiNet**: skip the built-in FujiNet and wait for another one
  (setting `fujinet_external`).
- **Listen address:port**: where to listen, for example `0.0.0.0:1985` (see
  [An external FujiNet](#an-external-fujinet); setting
  `fujinet_external_addr`).

## Display settings

- **Aspect ratio**: *TV (4:3)*, *Square pixels* or *Integer scale* (setting
  `aspect_mode`).
- **Scanlines**: draw the half scanlines of a monitor (setting `scanlines`).

<!-- context: prefs/Connection -->
## Preferences on Windows

On Windows there is no Preferences window: the Machine menu has the **Model**,
**Video** and **Slot 3** to **Slot 7** menus, the View menu has **Aspect Ratio**
and **Scanlines**, and **FujiNet ▸ Connection…** has **Use external FujiNet** and
the **Listen address:port**.

## The settings file

The settings file is `settings.ini` (see [Files and folders](#files-and-folders)).
Each setting above names its key. The others are `smooth_scaling` (the pixel
blending, which follows Scanlines).

# Debugger

The Debugger (<kbd>F12</kbd>) stops the machine and shows what it is doing. It has
two tabs, and closing it lets the machine go on as it was.

| Button | Does |
|---|---|
| Pause or Continue (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Into (<kbd>F7</kbd>) | One instruction |
| Over (<kbd>F8</kbd>) | Over a subroutine call |
| Out (<kbd>Shift+F8</kbd>) | On until the routine returns |
| Help | Opens the topic of the tab that shows |

<!-- context: debugger/cpu -->
## CPU

The 6502's registers, the disassembly with symbols, and a memory view; click a
line of the disassembly to toggle its breakpoint. A video page decodes a page of
memory as the CPU sees it right now: *Text 1*, *Text 2*, *Lo-res 1*, *Lo-res 2*,
*Hi-res 1* or *Hi-res 2*. It is a memory inspector, not what the video hardware
is scanning out, which is what makes it useful for finding the page a program is
drawing into.

<!-- context: debugger/fujinet -->
## FujiNet packets

Every SmartPort packet the card and FujiNet exchange, both ways, and the card's
state. **Clear** empties the list.

# Files and folders

The app keeps its settings in one file, and everything else in its data
folder:

<!-- only: linux -->

On Linux, the settings are `~/.config/fujinet-go-apple2/settings.ini` and
the data is in `~/.local/share/fujinet-go-apple2` (or under
`$XDG_CONFIG_HOME` and `$XDG_DATA_HOME`). The GNOME and KDE apps share them.
As a Flatpak, they are under `~/.var/app/`, in the app's own folder.

<!-- end -->

<!-- only: windows -->

On Windows, the settings are `%APPDATA%\fujinet-go-apple2\settings.ini` and
the data is in `%LOCALAPPDATA%\fujinet-go-apple2`.

<!-- end -->

<!-- only: macos -->

On a Mac, the settings are `~/.config/fujinet-go-apple2/settings.ini` and
the data is in `~/.local/share/fujinet-go-apple2`.

<!-- end -->

In the data folder:

| Folder | Has |
|---|---|
| `roms` | the system ROMs you imported |
| `fujinet` | FujiNet's settings, its SD folder (`SD`) and its data |

# Troubleshooting

- **The picture is blue bands on a blank screen**: the video mode is *Color (RGB
  Card/Monitor)* with no RGB card; choose another video mode.
- **The machine does not boot into CONFIG**: the slot 7 card must be *FujiNet*, and
  the Disk \]\[ in slot 6 must not boot first; check Preferences ▸ Machine.
- **Unsupported disk image**: the file is not one of `.dsk`, `.do`, `.po`, `.2mg`,
  `.nib`, `.woz` or `.hdv`.
- **A port is taken**: another program is using 1985 or 64001. Close the other
  FujiNet, or point the app at it with **Use external FujiNet**.
- **Keys don't reach the machine**: a dialog or another window is in front, or
  the key is one the app keeps (see
  [Controllers and keys](#controllers-and-keys)).
- **A gamepad does nothing**: it drives the paddles, so the program must read
  them.

# About and licences

FujiNet Go Apple II is free software under the GNU General Public License,
version 3 or later. Copyright © 2026 Thomas Cherryhomes.

- **AppleWin** (the Apple II emulator, GPL-2.0-or-later), as its libretro core, is
  the machine.
- **The FujiNet firmware** (GPL-3.0-or-later) runs inside the app.
- The Apple II system ROMs the app embeds are Apple's firmware; see
  `COMPLIANCE.md`. "Apple" and "Apple II" are trademarks of their owner, used to
  name the machine emulated; this project is not affiliated with it.

[fujinet.online](https://fujinet.online/) ·
[fujinet-go-apple2-desktop](https://github.com/FujiNetWIFI/fujinet-go-apple2-desktop)
