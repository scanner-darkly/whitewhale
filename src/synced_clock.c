#include "synced_clock.h"
#include "compiler.h"

static void sc_heartbeat_callback(void* o);
static void sc_update_period(synced_clock* sc, u32 tap);
static void sc_recalculate_intervals(synced_clock* sc);
static void sc_adjust_position(synced_clock* sc, u32 position);
static u32 sc_get_position(synced_clock* sc);

static void sc_heartbeat_callback(void* o) {
	synced_clock* sc = o;
	if (++sc->int_index >= sc->conf.mult) sc->int_index = 0;
	timer_remove(&sc->heartbeat);
	timer_add(&sc->heartbeat, sc->intervals[sc->int_index], &sc_heartbeat_callback, (void *)sc);
	(*(sc->callback))();
}

static void sc_update_period(synced_clock* sc, u32 tap) {
	u32 min = (sc->conf.period * 3) >> 2;
	u32 max = min + (sc->conf.period >> 1);
	if (tap < min || tap > max) sc->ext_taps_index = sc->ext_taps_count = 0;
	
	sc->ext_taps[sc->ext_taps_index] = tap;
	if (++sc->ext_taps_index >= SC_AVERAGING_TAPS) sc->ext_taps_index = 0;
	if (sc->ext_taps_count < SC_AVERAGING_TAPS) sc->ext_taps_count++;
	
	u64 total = 0;
	for (u8 i = 0; i < sc->ext_taps_count; i++) total += sc->ext_taps[i];
	sc->conf.period = total / (u64)sc->ext_taps_count;
}

static void sc_recalculate_intervals(synced_clock* sc) {
	u32 total = sc->conf.period * sc->conf.div;
	u32 interval = (total << 4) / (u32)sc->conf.mult;
	u32 acc = 0;
	u32 carryover = 0;
	for (u8 i = 0; i < sc->conf.mult; i++) {
		sc->intervals[i] = (interval + carryover) >> 4;
		carryover = (interval + carryover) & 15;
		acc += sc->intervals[i];
	}
	sc->intervals[sc->conf.mult - 1] += total - acc;
}

static void sc_adjust_position(synced_clock* sc, u32 position) {
	u32 remaining = position % (sc->conf.period * sc->conf.div);
	u8 expected_index = 0;
	while (remaining >= sc->intervals[expected_index] && expected_index < sc->conf.mult) {
		remaining -= sc->intervals[expected_index];
		expected_index++;
	}
	
	u32 max_delta = sc->intervals[0] >> 2;
	if (expected_index == (sc->int_index + 1) % sc->conf.mult && sc->heartbeat.ticksRemain < max_delta) {
		sc_heartbeat_callback((void *)sc);
	} else {
		sc->int_index = expected_index;
		sc->heartbeat.ticksRemain = sc->intervals[sc->int_index] - remaining;
	}
}

static u32 sc_get_position(synced_clock* sc) {
	u32 position = 0;
	for (u8 i = 0; i < sc->int_index; i++) position += sc->intervals[i];
	position += sc->heartbeat.ticks - sc->heartbeat.ticksRemain;
	return position;
}

void sc_process_tap(synced_clock* sc, u64 tick) {
	u64 elapsed = tick - sc->last_tick;
	sc->last_tick = tick;
	if (elapsed > (u32)60000) {
		sc->ext_index = 0;
		return;
	}
	
	if (++sc->ext_index >= sc->conf.div) sc->ext_index = 0;
	sc_update_period(sc, elapsed);
	sc_recalculate_intervals(sc);
	sc_adjust_position(sc, sc->conf.period * sc->ext_index);
}

void sc_update_div(synced_clock* sc, u8 div) {
	u32 pos = sc_get_position(sc);
	sc->conf.div = div ? div : 1;
	sc_recalculate_intervals(sc);
	sc_adjust_position(sc, pos);
}

void sc_update_mult(synced_clock* sc, u8 mult) {
	u32 pos = sc_get_position(sc);
	sc->conf.mult = mult ? mult : 1;
	sc_recalculate_intervals(sc);
	sc_adjust_position(sc, pos);
}

void sc_init(synced_clock* sc, sc_config* conf, sc_callback_t callback) {
	sc->conf.div = conf->div ? conf->div : 1;
	sc->conf.mult = conf->mult ? conf->mult : 1;
	sc->conf.period = conf->period;
	sc->callback = callback;

	sc->ext_index = 0;
	sc->int_index = 0;
	sc_recalculate_intervals(sc);
	
	sc->ext_taps_index = 0;
	sc->ext_taps_count = 0;
	sc->last_tick = 0;

	sc->int_index = sc->conf.mult - 1;
	sc_heartbeat_callback((void *)sc);
}

void sc_load_config(synced_clock* sc, sc_config* conf, u8 update_period, u8 update_div_mult, u8 from_clock) {
	u32 pos = from_clock ? 0 : sc_get_position(sc);
	if (update_div_mult) {
		sc->conf.div = conf->div ? conf->div : 1;
		sc->conf.mult = conf->mult ? conf->mult : 1;
		if (sc->conf.mult > SC_MAXMULT) sc->conf.mult = SC_MAXMULT;
	}
	if (update_period) {
		sc->conf.period = conf->period;
	}
	if (update_div_mult || update_period) {
		sc->ext_index = sc->int_index = 0;
		sc_recalculate_intervals(sc);
		if (from_clock)
			sc->heartbeat.ticksRemain = sc->intervals[sc->int_index];
		else
			sc_adjust_position(sc, pos);
	}
}

void sc_save_config(synced_clock* sc, sc_config* conf) {
	conf->div = sc->conf.div;
	conf->mult = sc->conf.mult;
	conf->period = sc->conf.period;
}
