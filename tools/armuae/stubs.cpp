// The rest of Amiberry 5.7.4, as far as its CPU core and JIT reach it, for
// armuae: no-ops for the debugger, input recording, save states, cartridges
// and GUI, and minimal stand-ins for time, RNG, threads and logging. The
// memory map, interrupts and the clock are in armuae.cpp.
#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "memory.h"
#include "custom.h"
#include "events.h"
#include "newcpu.h"
#include "savestate.h"
#include "inputrecord.h"
#include "ar.h"
#include "gui.h"
#include "uae.h"
#include "autoconf.h"
#include "statusline.h"
#include "audio.h"
#include "threaddep/thread.h"
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

// ---- preferences and chipset state the core reads ---------------------------
struct uae_prefs currprefs, changed_prefs;
int config_changed;
uae_u16 intreq, dmacon;
int vpos, maxvpos, maxhpos;
uae_u32 hsync_counter, vsync_counter;
volatile uae_atomic uae_int_requested, bsd_int_requested;
int quit_program;
struct gui_info gui_data;
uae_u32 timeframes;

void set_config_changed(int) { }
void fixup_cpu(struct uae_prefs *) { }
void custom_cpuchange(void) { }
void custom_prepare(void) { }
void reset_frame_rate_hack(void) { }
void check_prefs_changed_audio(void) { }
void audio_deactivate(void) { }
void statusline_clear(void) { }
void protect_roms(bool) { }
void mman_set_barriers(bool) { }
void target_cpu_speed(void) { }
void blitnasty(void) { }
void do_copper(void) { }
bool is_cycle_ce(uaecptr) { return false; }
void warpmode(int) { }
void notify_user(int) { }
void send_internalevent(int) { }
bool is_mainthread(void) { return true; }
void bsdsock_fake_int_handler(void) { }
void m68k_handle_trap(unsigned int) { }

// ---- input recording: off ------------------------------------------------------
int input_record, input_play, inputrecord_debug;
uaecptr wait_for_pc;
void inprec_startup(void) { }
int inprec_open(const TCHAR *, const TCHAR *) { return 0; }
bool inprec_prepare_record(const TCHAR *) { return false; }
void inprec_recorddebug_cpu(int, uae_u16) { }
void inprec_playdebug_cpu(int, uae_u16) { }

// ---- cartridges: none ----------------------------------------------------------
int hrtmon_flag, action_replay_flag;
uae_u32 hrtmem_start, hrtmem_size;
void action_replay_enter(void) { }
void action_replay_hide(void) { }
void hrtmon_enter(void) { }
void hrtmon_breakenter(void) { }
void hrtmon_hide(void) { }
int is_ar_pc_in_rom(void) { return 0; }
void a3000_fakekick(int) { }
bool cloanto_rom;
int uae_boot_rom_type;
uaecptr rtarea_base;

// ---- save states: never ----------------------------------------------------------
int savestate_state;
TCHAR savestate_fname[MAX_DPATH];
void savestate_init(void) { }
bool savestate_check(void) { return false; }
void savestate_rewind(void) { }
void savestate_restore_final(void) { }
bool savestate_restore_finish(void) { return false; }
void restore_state(const TCHAR *) { }
void save_u64_func(uae_u8 **, uae_u64) { }
void save_u32_func(uae_u8 **, uae_u32) { }
void save_u16_func(uae_u8 **, uae_u16) { }
uae_u64 restore_u64_func(uae_u8 **) { return 0; }
uae_u32 restore_u32_func(uae_u8 **) { return 0; }
uae_u16 restore_u16_func(uae_u8 **) { return 0; }

// ---- GUI, logging ------------------------------------------------------------------
void gui_led(int, int, int) { }
void gui_message(const TCHAR *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); }
void write_log(const TCHAR *fmt, ...) { if (!getenv("ARMUAE_LOG")) return; va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); }
void jit_abort(const TCHAR *fmt, ...) { va_list a; va_start(a, fmt); fprintf(stderr, "JIT abort: "); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); abort(); }

// ---- resets and restarts: armuae574.cpp decides ----------------------------------
void uae_reset(int, int) { }
void uae_restart(struct uae_prefs *, int, const TCHAR *) { }

// ---- time ------------------------------------------------------------------------------
frame_time_t vsyncmintime, vsyncmintimepre, vsyncmaxtime, vsyncwaittime, vsynctimebase, syncbase;
evt_t start_cycles;
bool event_wait;
int pissoff_value = 1024 * CYCLE_UNIT;
void sleep_millis(int ms) { usleep(ms * 1000); }
int sleep_millis_main(int ms) { usleep(ms * 1000); return 0; }

// ---- random numbers -------------------------------------------------------------------
static uae_u32 rnd_seed = 1;
uae_u32 uaerand(void) { rnd_seed = rnd_seed * 1103515245 + 12345; return rnd_seed >> 1; }
uae_u32 uaerandgetseed(void) { return rnd_seed; }
uae_u32 uaesetrandseed(uae_u32 s) { uae_u32 o = rnd_seed; rnd_seed = s; return o; }
void uaerandomizeseed(void) { rnd_seed = (uae_u32)time(NULL); }

// ---- threads: the core starts none in this build ------------------------------------
int uae_sem_init(uae_sem_t *, int, int) { return 0; }
int SDL_SemPost(SDL_sem *) { return 0; }
int SDL_SemWait(SDL_sem *) { return 0; }
int SDL_SemTryWait(SDL_sem *) { return 0; }
void SDL_Quit(void) { }
int uae_start_thread(const TCHAR *, uae_thread_function, void *, uae_thread_id *) { fprintf(stderr, "armuae: a thread was asked for\n"); return 0; }
SDL_threadID uae_thread_get_id(SDL_Thread *) { return 1; }
