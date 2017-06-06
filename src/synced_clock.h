#pragma once

#define SC_MAXMULT 16
#define SC_AVERAGING_TAPS 3

#include "types.h"
#include "timers.h"

typedef volatile struct {
	u8 div;
	u8 mult;
	u32 period;
} sc_config;

typedef void (*sc_callback_t)(void);

typedef volatile struct {
	sc_config conf;
	sc_callback_t callback;

	u32 intervals[SC_MAXMULT];
	u32 quarter;
	u8 ext_index;
	u8 int_index;

	u32 ext_taps[SC_AVERAGING_TAPS];
	u8 ext_taps_index;
	u8 ext_taps_count;
	u64 last_tick;
	u64 last_heartbeat;
	
	softTimer_t heartbeat;
} synced_clock;

void sc_init(synced_clock* sc, sc_config* conf, sc_callback_t callback);
void sc_load_config(synced_clock* sc, sc_config* conf, u8 update_period, u8 update_div_mult, u8 from_clock);
void sc_save_config(synced_clock* sc, sc_config* conf);
void sc_process_tap(synced_clock* sc, u64 tick);
void sc_update_div(synced_clock* sc, u8 div);
void sc_update_mult(synced_clock* sc, u8 mult);

/////////////////

sc_callback_t sc_notify;
volatile u16 sc_debug1, sc_debug2;
