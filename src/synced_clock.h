#pragma once

#include "types.h"
#include "timers.h"

#define SC_MAXMULT 16
#define SC_AVERAGING_TAPS 2

typedef void (*sc_callback_t)(void);

typedef volatile struct {
    softTimer_t heartbeat;
    sc_callback_t callback;
    u32 intervals[SC_MAXMULT];
    u8 interval;
    u32 quarter;
    u64 last_heartbeat;
} sc_clock;

typedef volatile struct {
    u32 taps[SC_AVERAGING_TAPS];
    u32 last_tap;
    u32 quarter;
    u8 index;
    u8 count;
    u64 last_tick;
} sc_taps;

typedef volatile struct {
    u32 period;
    u8 div;
    u8 mult;
    u8 phase_lock;

    sc_clock clock;
    sc_taps taps;
    
    u8 div_index;
    u32 phase_offset;
    
    u32 new_period;
    u8 new_div;
    u8 new_mult;
    softTimer_t changes_from_first_tap;
    softTimer_t changes_from_clock;
    softTimer_t changes_from_config;
} synced_clock;

void sc_init(synced_clock* sc, u8 div, u8 mult, u32 period, u8 phase_lock, sc_callback_t callback);
void sc_start(synced_clock* sc);
void sc_stop(synced_clock* sc);
u32 sc_process_clock(synced_clock* sc, u8 use_average);
void sc_update_divmult(synced_clock* sc, u8 div, u8 mult, u8 from_timer);
void sc_set_lock_mode(synced_clock* sc, u8 phase_lock);