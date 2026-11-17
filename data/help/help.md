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
- **Controllers** (<kbd>F9</kbd>): see [The Controllers window](#the-controllers-window).
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
window manager. They are also the game port's buttons 0 and 1, which the
Controllers window shows lit. <kbd>Ctrl</kbd> and <kbd>Shift</kbd> are the Apple
II's. The numeric keypad is the joystick (see
[Changing the controls](#changing-the-controls)). The app keeps only these keys
for itself:

| Shortcut | Does |
|---|---|
| <kbd>Ctrl+Shift+H</kbd> | Help |
| <kbd>F9</kbd> | Controllers window |
| <kbd>F11</kbd> | Fullscreen |
| <kbd>F12</kbd> | Debugger |

<!-- only: macos -->

On a Mac the Controllers window is also <kbd>⌘K</kbd>.

<!-- end -->

In the debugger, <kbd>F9</kbd> toggles a breakpoint instead.

<!-- only: gnome -->

<kbd>F10</kbd> opens the main menu.

<!-- end -->

<!-- only: linux -->

Key combinations with the Super (Windows) key are the desktop's.

<!-- end -->

<!-- context: controllers -->
## The Controllers window

The Controllers window (<kbd>F9</kbd>) draws what is plugged into the Apple II's
one game port, as the real thing looks. It floats on its own at a fixed size,
stays hidden until you ask for it and hides rather than closes. The game port
carries one joystick or two paddles, so there is no second joystick to show.
**Plugged in** picks which:

- **Joystick**: the Apple joystick of the //e and //c. A square cream base with
  two orange square buttons, button 0 on the left and button 1 on top, and a
  black stick in a recessed square well. The **Colour** choice is **Beige
  (IIe)** or **Platinum (IIc)**; it only changes how the joystick looks.
- **Paddles**: two Apple Hand Controllers, each with a ribbed knob and an orange
  button. Paddle 0 and paddle 1 are the machine's PDL0 and PDL1.

Under the picture are the **Reset** and **Power Cycle** buttons (the same as the
Machine menu: Power Cycle boots the card in slot 7 into CONFIG), a row for each
gamepad, and the **Map**, **Clear**, **Defaults** and **Show bindings** buttons,
a **Keyboard** check box and **Help**. There is no Reset to CONFIG button, and
Stop in Debugger has no button: bind a key to it (see
[Changing the controls](#changing-the-controls)).

The controls that are held light up in the accent colour, whoever holds them:
the keyboard, a gamepad or the mouse. The Alt keys light button 0 and button 1.

| With the mouse | Does |
|---|---|
| Press a button | Holds it for as long as the mouse button is down |
| Drag on the joystick | Moves the stick as far as the pointer goes, in any direction, not in eight steps |
| Drag on a knob | Turns it to where the pointer is |
| Wheel over a knob | Nudges it |
| Drag off a control | Lets it go |
| Release on Reset, Power Cycle | Runs it |

The stick and the knobs are analog: the machine reads a position, and a knob
stays where you leave it. Switching to another window lets go of everything the
mouse and the keyboard hold, and so does changing **Plugged in**. Keys typed in
the Controllers window go to the machine too, with the Alt keys still Open Apple
and Closed Apple.

**Show bindings** prints the key or gamepad button on each control. It is
remembered (setting `controllers_show_bindings`), as are **Plugged in**
(`controller_mode`, *joystick* or *paddles*) and **Colour** (`controller_model`).
The window can be opened at launch with `APPLE2_OPEN_CONTROLLERS=1`.

## Keyboard view

The **Keyboard** check box adds the Apple //e's own keyboard under the controller:
its 63 keys as printed, with Esc, Tab, Control, Shift, Caps Lock, Delete,
Return, the four arrow keys, the Open Apple and Option (Closed Apple) keys and a
small Reset key above Esc. Click a key to type it on the Apple II; a key sends
when it is pressed and lights for a moment. <kbd>Shift</kbd> and
<kbd>Control</kbd> latch: they stay lit until the next key, and let go after it.
<kbd>Caps Lock</kbd> toggles, and starts on, as the //e does. The Open Apple and
Option keys are held while the mouse button is down, and are buttons 0 and 1. The
keyboard is off by default, and the choice is remembered (setting
`controllers_keyboard`; Caps Lock is `caps_lock`).

## Changing the controls

**Map** lets you change a control: click it, then click the control to change
(on the joystick, the side of the well for a direction; on a knob, its left half
for counter-clockwise, its right half for clockwise and its middle for a
gamepad axis), then press the key or the gamepad button. Each control has one
key and one gamepad button. A key already on another control is taken from it.
**Clear** takes the picked control's key and button away, **Defaults** puts every
control back, and the Escape key leaves Map. The keys <kbd>Alt</kbd>,
<kbd>Meta</kbd>, <kbd>Super</kbd>, <kbd>F9</kbd>, <kbd>F11</kbd> and <kbd>F12</kbd>
cannot be bound. Stop in Debugger can be bound to a key: use **Debugger key**
where the window has it, or pick it from the Map list.

The defaults are:

| Control | Key | Gamepad |
|---|---|---|
| Joystick left, right, up, down | Keypad 4, 6, 8, 2 | D-pad; the left stick, proportionally |
| Joystick button 0 | Keypad 0 | South (A) |
| Joystick button 1 | Keypad Enter | East (B) |
| Paddle 0 left, right, button | Keypad 7, 9, 5 | D-pad left and right; left stick X; South |
| Paddle 1 left, right, button | Keypad 1, 3, Keypad . | the same on its own pad |

The numeric keypad steers the joystick instead of typing digits. A key bound to
the other mode's control does nothing until you change **Plugged in**. The
bindings are remembered (setting `bindings`).

## Gamepads

Gamepads are picked up as they are plugged in, and so are plain USB joysticks
that are not known as gamepads, such as the USB adapters for Apple II joysticks.
A pad's stick moves the joystick proportionally (a dead zone is taken off so the
stick still reaches its full travel); its buttons are the game port's buttons.
In paddles mode a pad's left stick sets its paddle's knob and the knob stays
where it was left.

Each gamepad has a row in the Controllers window with its name and a choice of
*Automatic* and *Joystick* in joystick mode, or *Automatic*, *Paddle 0* and
*Paddle 1* in paddles mode. Automatic gives the first pad the joystick, or
paddle 0, and the second pad paddle 1. A pad you choose a port for is
remembered by its identity (setting `pad_assign`), so it comes back to it when
it is plugged in again, and unplugging one pad never moves another. A pad that
has no port drives nothing. When a pad is unplugged or moves, everything it held
is let go. A message shows in the window when a pad is connected or removed.

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

<!-- context: prefs/Input -->
## Input settings

<!-- only: gnome kde macos -->

The Input page has:

- **Controller** (*Joystick* or *Paddles*) and the joystick's colour (*Beige
  (IIe)* or *Platinum (IIc)*): they apply at once, and the Controllers window
  follows them (settings `controller_mode` and `controller_model`).
- **Enable gamepads**: turn the gamepad thread off or on; applies by restarting
  the session when Preferences closes (setting `enable_gamepad`).
- A row for each gamepad with its name and the port it plays: *Automatic* or
  *Joystick*, or in paddles mode *Paddle 0* or *Paddle 1* (see
  [Gamepads](#gamepads)).
- **Reset Controls**: every key and gamepad button back to its default.
- **Open Controllers**: opens the Controllers window.

<!-- end -->

<!-- only: windows -->

On Windows these are in **View ▸ Input Settings…**, a window of its own: the
**Controller** choice (*Joystick* or *Paddles*; the joystick's colour is chosen in
the Controllers window), **Enable gamepads** (applies when the window closes), a row for each gamepad, **Reset Controls** and **Open
Controllers**. `APPLE2_OPEN_INPUT_SETTINGS=1` opens it at launch.

<!-- end -->

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
the **Listen address:port**. The View menu also has **Input Settings…**, with the
gamepad and controller settings: see [Input settings](#input-settings).

## The settings file

The settings file is `settings.ini` (see [Files and folders](#files-and-folders)).
Each setting above names its key. The others are `smooth_scaling` (the pixel
blending, which follows Scanlines), `bindings` and `pad_assign` (the controls and
the gamepads' ports, see [Changing the controls](#changing-the-controls)), and
`enable_gamepad`, `controller_mode` and `controller_model` (the Input page), and
`controllers_show_bindings`, `controllers_keyboard` and `caps_lock` (the
Controllers window's choices).

# Debugger

The Debugger (<kbd>F12</kbd>) stops the machine and shows what it is doing.
It has ten tabs, and the registers show under all of them; closing it lets the
machine go on as it was. Opening it stops the machine. **Pop Out** moves any
tab into a window of its own, and **Dock** (or closing that window) puts it
back.

| Button | Does |
|---|---|
| Run or Stop (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Step (<kbd>F7</kbd>) | One instruction |
| Step Over (<kbd>F8</kbd>) | Over a `JSR` or `BRK` |
| Step Out (<kbd>Shift+F8</kbd>) | On until the routine returns (`RTS` or `RTI`) |
| Frame (<kbd>Shift+F7</kbd>) | On to the next frame |
| Run to Cursor | On to the selected line |
| Load Symbols | Reads `.SYM`, `.lbl`, `.sym` and `.map` files |
| Save | Saves the disassembly, memory or registers to a file |
| Help | Opens the topic of the tab that shows |
| Pop Out | Moves the tab that shows into its own window |

The screen and the sound go on while the Debugger is open and the machine
runs; only a stop freezes them. The soft switches are memory on the Apple II
(`$C000` to `$C0FF`), so a breakpoint on one of them is an ordinary read or
write breakpoint.

<!-- context: debugger/console -->
## Console

The Console takes commands, with completion on <kbd>Tab</kbd> and recall on
<kbd>Up</kbd> and <kbd>Down</kbd>. Numbers are `$C000`, `0xC000`, `C000h`,
`49152` or `%1010`; expressions can use the registers `a`, `x`, `y`, `p`, `sp`
and `pc`, the flags `cf`, `zf`, `if`, `df`, `bf`, `vf` and `nf`, `[addr]` and
`{addr}` for memory, labels, and C's operators. `help` lists the commands.

| Command | Does |
|---|---|
| `step`, `over`, `out` | One instruction; over a call; to the return |
| `run`, `go`, `stop` | Go on or break |
| `runto` | Runs to an address |
| `scanline`, `frame` | Runs some beam lines or frames |
| `break`, `bp` | An execute breakpoint |
| `bpr`, `bpw` | Memory reads or writes, soft switches included |
| `delete`, `enable`, `clear` | Manage breakpoints (also `disable`, `breaks`) |
| `print` | Evaluates an expression |
| `regs`, `set` | Shows or sets the registers |
| `mem`, `dump`, `poke` | Dumps memory; writes bytes (take a bank prefix: `mem aux:$0400 32`) |
| `bank` | The memory banks and which this machine has |
| `video` | The video mode, soft switches and timing (`video colours` adds the palette usage) |
| `io`, `map` | The soft switches, the slots and the memory map |
| `fujinet` | The FujiNet link |
| `label`, `labels`, `symbols` | Name an address; load labels |
| `disasm`, `dis` | Disassembles (`disasm rom:$FA62`) |
| `trace` | The instruction trace |
| `watch`, `unwatch` | The watch list: `watch count`, `unwatch 0` |
| `find` | Searches memory for bytes (`find A9 00`) or text (`find text HELLO`); `find aux: text HELLO` searches one bank |
| `save` | Saves state to a file |

<!-- context: debugger/disassembly -->
## Disassembly

The 6502 (the 65C02 on an Enhanced Apple //e) disassembled at the PC, with the
labels from AppleWin's symbol files and the soft switch names. The current
instruction is in the app's accent colour and breakpoints are red dots (grey
when disabled). Click a line to select it; double-click, <kbd>F9</kbd> or the
gutter toggles its breakpoint. **Follow PC** keeps the current line in view,
and every stop brings the view back to it, even if you had scrolled away.

The registers and flags sit in a strip under the tabs. Edit a value while
stopped and press <kbd>Enter</kbd>; the stack pointer takes `$F0` or `$01F0`.

<!-- context: debugger/memory -->
## Memory

The 64K the CPU sees right now (bank switching, the language card, auxiliary
memory and the ROMs included), 16 bytes to a row; looking never touches a soft
switch. **Go To** takes an address, a label, an expression or a range
(`$0400-$07FF`, `count..count+3`) and highlights what it found. **Poke**
writes bytes; ROM ignores them. The search bar finds hex bytes (`A9 00`) or
text (not case-sensitive) with **Find Next** and **Find Previous**, wrapping
once; the machine has to be stopped.

### Memory banks

The console can also look at a bank directly, whatever the soft switches say,
by putting a prefix in front of the address: `main:`, `aux:` (auxiliary RAM),
`lc1:` and `lc2:` (the language card's RAM banks at `$D000-$DFFF`), `lchi:`
(language card RAM at `$E000-$FFFF`), `rom:` (the system ROM) and, for the
auxiliary side's language card, `alc1:`, `alc2:` and `alchi:`. So `mem aux:$0400`
dumps auxiliary text page 1, `poke lc2:$D000 $60` edits bank 2, `disasm rom:$FA62`
reads the ROM even while the language card shows RAM, and `print [aux:$0400]`
and `print {aux:$0400}` use a bank in an expression. The prefix works in `find`
and in watches too (`watch aux:$0400`). `bank` lists them and says which this
machine has: an Apple II+ has no auxiliary RAM, and the language card only if one is
fitted. A bank shows `$FF` for addresses it does not cover (main and aux stop
at `$BFFF`; `rom:` starts at `$D000`, or `$C100` on a //e). ROM ignores
writes. RamWorks and Saturn extra banks are not shown.

<!-- context: debugger/breakpoints -->
## Breakpoints

Execute breakpoints with conditions, address ranges and hit counts, and memory
read and write watchpoints, each of which can be disabled. Watch a soft switch
the same way: a read breakpoint on `$C030` stops at every click of the speaker.
A condition can use `value` and `addr`, and on a write `old`, the byte that was
there before: `value != old` stops only when a write changes something. Read
and write breakpoints stop after the instruction that touched the address.

They see an instruction's data accesses only. Opcode and operand fetches, stack
pushes and pulls (`PHA`, `JSR`, `RTS`) and the pointer fetches of indirect
addressing are not watched, and neither is the operand of an immediate
instruction (`LDA #$00`). The line at the top says why the machine last
stopped. **Add** makes a breakpoint, **Clear Hits** zeroes the counts and
**Clear All** removes them.

<!-- context: debugger/watches -->
## Watches

Things to keep an eye on: type an address, a label, an expression or a range
and press **Add**. Each shows its value while the machine is stopped (`-` while
it runs, `?` if it means nothing yet, which a later **Load Symbols** may fix).
**Memory** shows it in the Memory tab, **Break on Change** stops the machine
when something writes a different value there, and **Remove** drops it.

<!-- context: debugger/slots -->
## Slots and I/O

The machine and its CPU, the soft switches (text, mixed, page 2, hi-res,
80 columns, 80STORE, the alternate character set, RAMRD, RAMWRT, ALTZP, the
language card's bank and whether it reads and writes RAM, INTCXROM and
SLOTC3ROM), the card in every slot (the FujiNet SmartPort card is marked) and
the memory map as the CPU sees it now: main or auxiliary RAM, the language
card or the ROM at `$D000`, the slot ROMs or the internal ROM at `$C100`.

<!-- context: debugger/trace -->
## Trace

Every instruction the CPU runs while the Debugger is open, live. **Record**
turns it on (it slows the machine a little, so it starts off), **Clear**
empties it, and the filter shows everything, only the jumps, branches, calls
and returns, or only a range of addresses. Trace and History share one
recording.

<!-- context: debugger/history -->
## History

The same recording, read at a stop: the lead-up to a breakpoint or a pause,
oldest first, with the registers at each instruction. Turn **Record** on
before you run to the place you want to look at.

<!-- context: debugger/video -->
## Video

The video inspector: the picture the machine is showing, and the memory behind it.

**View** picks what is drawn:

- *Screen* is the last finished frame, exactly as the machine displayed it (its
  size depends on the video mode and on a Video HD card).
- *Decoded* redraws video memory through the soft switches, always at 560 by
  192: the 40-column modes are doubled across so that every byte cell is 14
  pixels wide, and the 80-column and double modes (7 pixels per byte, aux
  memory first) line up on the same grid. It shows Text 40, Text 80 (aux and
  main columns interleaved), Lo-res, Double lo-res, Hi-res and Double hi-res,
  page 1 or 2, with the bottom four lines as text in mixed mode, and
  lowercase/MouseText when ALTCHARSET is on. Flashing characters are drawn
  steady.

**Follow screen** (on by default) makes Decoded track the mode, page and mixed
setting the soft switches show right now. Turn it off to choose the mode and
page yourself and look at a page the program is drawing into before it flips
to it; the memory is read the same way whatever the machine is showing.
**Zoom** is 1 to 4, or Fit.

**Overlays** draw over either view: *Grid* (7 by 8 text cells, 7 by 4 lo-res
blocks, hi-res byte cells), *Mixed split* (the line below which mixed mode
shows text), *Beam* (the scanline being drawn; the display is 192 of the
frame's 262 lines), *Heat* (bytes of the displayed page written lately, fading
over about sixteen refreshes), and *Changed* (bytes that changed since the last
refresh).

Move the mouse over the picture to see the byte under it in the **Pixel** inspector: the
cell, its main and aux addresses and values, and what they mean (a character
and whether it is inverse, flashing or normal; a lo-res colour nibble, rotated
once for the aux half of double lo-res; a hi-res pixel's bit and the palette
bit that picks violet/green or blue/orange; a double hi-res dot group's
colour). On the Screen view the pixel is mapped onto the same cell. **Click**
to pin that byte and click it again to unpin; a pinned byte keeps updating as
the machine runs.

**Break on writes** (or a double-click on the picture) adds Write breakpoints
on the inspected byte: its main address, and its aux address too in the
80-column and double modes (the CPU writes both through the same address, with
RAMWRT or 80STORE choosing the bank). **Show in Memory** jumps the Memory tab to
the byte, and **Unpin** drops the pin.

The tables:

- **Switches** lists the video soft switches (TEXT, MIXED, PAGE2, HIRES, 80COL,
  80STORE, ALTCHARSET and DHIRES, which is AN3 clear), whether each is on, how
  many refreshes ago it last changed, and the $C0xx addresses that set and clear
  it. Break on a switch stops when either address is read or written.
- **Colours** counts the pixels of the Decoded picture in each colour: the 16
  lo-res colours, the six hi-res colours, or black and white for text. The
  inspected pixel's colour is marked.
- **Timing** shows the mode, the cycles per frame, where the beam is (line and
  horizontal position), whether the machine is in vertical blank, and the frame
  and cycle counts.

The console's `video` command prints the same tables; `video switches`,
`video colours` and `video timing` print one.

<!-- context: debugger/fujinet -->
## FujiNet packets

Every SmartPort packet the card and FujiNet exchange, both ways, and the card's
state, recorded from the moment the machine starts. **Clear** empties the list.

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
