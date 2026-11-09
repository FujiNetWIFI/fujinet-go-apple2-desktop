/*
 * apple2_host -- the libretro frontend for the staged AppleWin core.
 *
 * There is no RetroArch here: AppleWin's libretro frontend is built as a
 * STATIC library and linked whole into libapple2session, so the retro_*
 * entry points are called directly rather than dlsym'd out of a core.
 *
 * This is the ONLY translation unit that includes AppleWin headers. It must
 * not include apple2session.h -- the session talks to it through this C API
 * and nothing else, which is what keeps AppleWin's C++ (and its `Apple2*`,
 * `A2_*`, `CT_*` namespace) out of the rest of the tree.
 *
 * Ported from fujinet-go-apple2's app/src/main/cpp/apple2_host.cpp, which
 * does the same job for the Android app; the differences are the logging
 * seam (Android had __android_log_print wired in directly) and nothing else.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLE2_HOST_H
#define APPLE2_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest frame the core can produce: 640x400 with a VidHD card in slot 3
 * (kVideoWidthIIgs/kVideoHeightIIgs), against 560x384 for a stock machine.
 * Callers size their buffers to the max and use the width/height the frame
 * sink reports. */
#define APPLE2_HOST_MAX_WIDTH  640
#define APPLE2_HOST_MAX_HEIGHT 400

/* Native pixel format is XRGB8888 -- the core is told so at
 * RETRO_ENVIRONMENT_SET_PIXEL_FORMAT and no other format is accepted. Rows
 * are tightly packed (the host repacks away the core's pitch), so a frame is
 * exactly width*height uint32_t. */
typedef void (*Apple2FrameSink)(const uint32_t *xrgb8888, int width, int height,
                                void *user);
void apple2host_set_frame_sink(Apple2FrameSink sink, void *user);

/* Diagnostics from the core and from this layer. level: 0 debug, 1 info,
 * 2 warn, 3 error. With no sink installed, warnings and errors go to stderr
 * and the rest is dropped. */
typedef void (*Apple2LogSink)(int level, const char *msg, void *user);
void apple2host_set_log_sink(Apple2LogSink sink, void *user);

/* Core options (libretro "variables"), e.g. key "applewin_slot7" value
 * "FujiNet". MUST be set before apple2host_core_start(): the core registers
 * its own defaults through SET_VARIABLES, which deliberately does not clobber
 * a key already present here, and it reads them at load time. */
void apple2host_set_variable(const char *key, const char *value);

/* Registers the libretro callbacks, runs retro_init / retro_load_game(NULL)
 * and reads the initial geometry. Returns false if the core refused to load.
 * Everything here and every *_run_frame/_stop call must happen on the same
 * thread. */
int  apple2host_core_start(void);
/* One retro_run(): one emulated ~60Hz frame, ending in a frame-sink call. */
void apple2host_core_run_frame(void);
void apple2host_core_stop(void);

/* Current geometry, i.e. what the next frame will most likely be. */
void apple2host_get_geometry(int *width, int *height);
/* The core's declared frame rate (retro_get_system_av_info timing.fps), used
 * to pace the emulator thread. 0 before the core has started. */
double apple2host_frame_rate(void);

/* Warm reset (Ctrl-Reset -- drops to BASIC) and cold reset / power cycle
 * (Ctrl-OpenApple-Reset -- re-runs the autostart ROM and reboots). */
void apple2host_ctrl_reset(void);
void apple2host_power_cycle(void);

/* ---- debugger seam -------------------------------------------------------
 * All of these touch live core state, so they must be called from the
 * emulator thread -- in practice from inside the session's frame loop, with
 * the loop parked. They are the foundation the debugger engine builds on.
 *
 * AppleWin turns out to expose exactly what is needed: CpuExecute() documents
 * "uCycles: =0 : Do single step", so there IS a clean single-instruction
 * entry point, and the register file and memory image are plain globals. */
typedef struct {
    uint8_t  a, x, y, ps;
    uint16_t pc, sp;
    uint8_t  jammed;    /* NMOS 6502 has executed an illegal opcode */
} apple2host_regs;

void apple2host_get_regs(apple2host_regs *out);
void apple2host_set_regs(const apple2host_regs *in);

/* Execute exactly one instruction and return the cycles it took. */
unsigned apple2host_step_instruction(void);

/* Read/write the CPU's current memory view (what the 6502 would see at that
 * address right now, respecting bank switching: AppleWin's
 * ReadByteFromMemory / WriteByteToMemory, which also cover the //e with an
 * empty aux slot, where the flat image is not valid). Reads are non-intrusive:
 * they never run the soft-switch handlers, so inspecting $C000-$CFFF in a
 * debugger cannot toggle hardware. ROM ignores writes. */
void apple2host_read_mem(uint16_t addr, uint8_t *out, unsigned len);
void apple2host_write_mem(uint16_t addr, const uint8_t *in, unsigned len);

/* ---- the stepped path ----------------------------------------------------
 * While the debugger engine is engaged the session steps the CPU itself
 * (apple2host_step_instruction) instead of calling apple2host_core_run_frame().
 * retro_run() does more than run the CPU, and the stepped path must do all of
 * it or the screen, the sound and the keyboard stop:
 *   step_begin()       variable / input polling         (start of a frame)
 *   step_work(cycles)  the cards and the speaker catch up with the CPU,
 *                      and the frame's cycle counter advances; call it with
 *                      the cycles the stepped instructions took, in batches
 *   step_end()         present the screen, hand the audio on, refresh the
 *                      cached FujiNet device count      (end of a frame)
 * present_screen() alone re-presents the picture (a stop). */
void apple2host_step_begin(void);
void apple2host_step_work(unsigned cycles);
void apple2host_step_end(void);
void apple2host_present_screen(void);

/* ---- debugger: data accesses ---------------------------------------------
 * A hook run from inside the running CPU loop for every data read (after the
 * value is fetched) and write (BEFORE the store, so the old byte can still be
 * read) an instruction makes. Opcode and operand fetches, stack pushes/pulls
 * and indirect-pointer fetches do not go through it. NULL (the default) costs
 * one null-pointer test per data access. Called on the emulator thread. */
typedef void (*Apple2AccessHook)(uint16_t addr, uint8_t value, int write);
void apple2host_set_access_hook(Apple2AccessHook hook);

/* ---- debugger: machine state --------------------------------------------- */
int      apple2host_is_65c02(void);          /* Enhanced //e (65C02) */
uint64_t apple2host_cycles(void);            /* cycles since power-on */
unsigned apple2host_cycles_per_frame(void);  /* 17030 NTSC, 19656 PAL */
unsigned apple2host_lines_per_frame(void);   /* 262 NTSC, 312 PAL */
/* The beam: scanline of the frame in progress (0-based), and the video clock
 * within the line. */
void     apple2host_get_beam(unsigned *line, unsigned *horz);
/* "Apple //e", "Enhanced Apple //e", "Apple ][+" ... */
const char *apple2host_model_name(void);
/* AppleWin's MF_* memory-mode flags (Memory.h), re-exposed so the engine does
 * not need AppleWin's headers. */
#define APPLE2HOST_MF_80STORE   0x00000001u
#define APPLE2HOST_MF_ALTZP     0x00000002u
#define APPLE2HOST_MF_RAMRD     0x00000004u
#define APPLE2HOST_MF_RAMWRT    0x00000008u
#define APPLE2HOST_MF_BANK2     0x00000010u  /* language card bank 2 at $D000 */
#define APPLE2HOST_MF_HIGHRAM   0x00000020u  /* language card RAM read */
#define APPLE2HOST_MF_HIRES     0x00000040u
#define APPLE2HOST_MF_PAGE2     0x00000080u
#define APPLE2HOST_MF_SLOTC3ROM 0x00000100u
#define APPLE2HOST_MF_INTCXROM  0x00000200u
#define APPLE2HOST_MF_WRITERAM  0x00000400u  /* language card RAM write */
uint32_t apple2host_mem_mode(void);
/* AppleWin's VF_* video flags. */
#define APPLE2HOST_VF_80COL     0x00000001u
#define APPLE2HOST_VF_DHIRES    0x00000002u
#define APPLE2HOST_VF_HIRES     0x00000004u
#define APPLE2HOST_VF_80STORE   0x00000008u
#define APPLE2HOST_VF_MIXED     0x00000010u
#define APPLE2HOST_VF_PAGE2     0x00000020u
#define APPLE2HOST_VF_TEXT      0x00000040u
uint32_t apple2host_video_mode(void);
int      apple2host_altcharset(void);
/* Slots 0-7 and 8 (the //e's auxiliary slot): the card's name ("" if empty)
 * into name; returns 1 for the FujiNet SmartPort card, else 0. */
int      apple2host_slot_card(int slot, char *name, int namesz);
int      apple2host_has_aux(void);           /* slot 8 exists (a //e) */

/* ---- FujiNet trace seam --------------------------------------------------
 * Every SmartPort request the card sends FujiNet over SLIP, and the reply:
 * kind 0 is a request, 1 its reply, 2 no reply in time (buf NULL). seq is
 * the request's sequence number; buf is the whole packet, SLIP-decoded.
 *
 * Called on the emulator thread (the card) AND on the SmartPort Listener's
 * thread (its INIT scan when FujiNet connects), so the sink must lock. Set it
 * before apple2host_core_start: the card's constructor starts the Listener.
 * NULL removes it; this layer serialises the sink against its callers, so
 * once that returns the old sink is never called again. */
typedef void (*Apple2SpTraceSink)(int kind, uint8_t seq, const uint8_t *buf,
                                  size_t len, void *user);
void apple2host_set_sp_trace(Apple2SpTraceSink sink, void *user);

/* Where the card's SmartPort-over-SLIP Listener binds: an IPv4 literal and a
 * port. NULL/"" and 0 mean the default, 127.0.0.1:1985. Takes effect when the
 * card is inserted, so set it before apple2host_core_start(). */
void apple2host_set_slip_listener(const char *host, int port);

/* SmartPort devices FujiNet has registered with the card's Listener; 0 means
 * no link. Read from the Listener on the emulator thread after each frame
 * and cached, so any thread may call this. */
int apple2host_fujinet_devices(void);

/* Pull interleaved stereo int16 @ 44100Hz. Blocks briefly for a full block
 * (bounded, ~1.5 block durations) then silence-pads, so the caller always
 * gets max_samples back and an underrun is a short gap rather than a stall.
 * max_samples counts int16 samples, not frames. */
int  apple2host_fill_audio(int16_t *out, int max_samples);
void apple2host_audio_set_active(int active);
void apple2host_clear_audio(void);

/* Keyboard, in libretro terms: keycode is RETROK_*, character the translated
 * Unicode value (0 for none, and 0 for Ctrl combos so the core resolves them
 * from the keycode), mods a RETROKMOD_* bitmask. See core/src/input_map.c. */
void apple2host_inject_key(int down, unsigned keycode, uint32_t character,
                           uint16_t mods);

/* Port 0 is an analog controller so the stick drives the Apple II paddles
 * (PDL0/PDL1) proportionally; buttons are read as JOYPAD A/B either way.
 * id 8 (RETRO_DEVICE_ID_JOYPAD_A) is Apple button 0 / Open Apple,
 * id 0 (RETRO_DEVICE_ID_JOYPAD_B) is Apple button 1 / Closed Apple.
 * axis 0 = X, 1 = Y, full scale -32768..32767. */
void apple2host_set_joystick_button(int port, int id, int pressed);
void apple2host_set_joystick_axis(int port, int axis, int16_t value);

/* --- bank seam ---------------------------------------------------------------
 * Memory banks read straight from AppleWin's own storage (main, aux, language
 * card RAM, ROM image), never through the soft-switch handlers. Where the
 * flat 'mem' cache holds the live copy of a page, that copy is used (so
 * unflushed writes are seen); otherwise the backing store. Emulator thread
 * only. Not covered: RamWorks / Saturn extra banks, custom ROM overrides. */
#define APPLE2HOST_BANKSTORE_MAIN    0  /* main RAM, offset = address, $0000-$BFFF */
#define APPLE2HOST_BANKSTORE_AUX     1  /* aux RAM, same */
#define APPLE2HOST_BANKSTORE_LCMAIN  2  /* main LC RAM 16K: +0 bank1, +$1000 bank2, +$2000 $E000-$FFFF */
#define APPLE2HOST_BANKSTORE_LCAUX   3  /* aux LC RAM, same layout */
#define APPLE2HOST_BANKSTORE_ROM     4  /* system ROM: offset = address - $C000; $0000-$0FFF
                                           is the //e internal $C000-$CFFF ROM (only $C100-$CFFF
                                           meaningful), $1000-$3FFF is $D000-$FFFF */
#define APPLE2HOST_BANKSTORE_SIZE(s) ((s) <= 1 ? 0xC000u : 0x4000u)
int      apple2host_bank_present(int store);                       /* 1 if the machine has it */
/* Return the number of bytes read/written (0 if absent or outside the store).
 * Writes to ROM are ignored (return 0). */
unsigned apple2host_bank_read(int store, unsigned offset, uint8_t *out, unsigned len);
unsigned apple2host_bank_write(int store, unsigned offset, const uint8_t *in, unsigned len);
/* --- end bank seam --- */

/* --- vidtab seam --- */
/* Physical video RAM, whatever RAMRD/RAMWRT/80STORE say: aux=0 main, aux=1 aux.
 * Returns bytes read (0 and zero-filled when the bank does not exist). */
unsigned apple2host_read_video_ram(int aux, uint16_t addr, uint8_t *out, unsigned len);
/* --- end vidtab seam --- */

#ifdef __cplusplus
}
#endif

#endif /* APPLE2_HOST_H */
