/* Menu/command identifiers for the Windows frontend. */
#ifndef APPLE2_WIN_RESOURCE_H
#define APPLE2_WIN_RESOURCE_H

#define IDI_APPICON         101

#define IDM_RESET           201
#define IDM_POWER_CYCLE     202
#define IDM_IMPORT_MEDIA    203
#define IDM_IMPORT_ROMS     204
#define IDM_FUJINET_CONFIG  205
#define IDM_FUJINET_LOG     206
#define IDM_FULLSCREEN      207
#define IDM_DEBUGGER        208
#define IDM_SCANLINES       209
#define IDM_ABOUT           210
#define IDM_EXIT            211

/* The machine, video and slot menus are built from the option names the core
 * registers (apple2session_machine_name() and friends), so their command IDs
 * are allocated as base+index rather than one at a time. 100 apart because
 * the longest of these lists -- the machine models -- has ten entries and
 * more could be added upstream. */
#define IDM_OPT_SPAN        100
#define IDM_MACHINE_BASE    1000
#define IDM_VIDEO_BASE      1100
#define IDM_SLOT3_BASE      1200
#define IDM_SLOT4_BASE      1300
#define IDM_SLOT5_BASE      1400
#define IDM_SLOT6_BASE      1500
#define IDM_SLOT7_BASE      1600
#define IDM_ASPECT_BASE     1700

#endif
