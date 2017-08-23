#include "print_funcs.h"

#include "synced_clock.h"
#include "compiler.h"
#include "init_trilogy.h"
#include "interrupts.h"

static void sc_heartbeat_callback(void* o);
static void sc_update_period(synced_clock* sc, u32 tap);
static void sc_recalculate_intervals(synced_clock* sc);
static void sc_adjust_position(synced_clock* sc, u32 position);
static u32 sc_get_position(synced_clock* sc);
static void sc_update_conf(synced_clock* sc);

static void sc_heartbeat_callback(void* o) {
	synced_clock* sc = o;
	if ((get_ticks() - sc->last_heartbeat) < sc->quarter) return;

    sc->last_heartbeat = get_ticks();
	if (++sc->int_index >= sc->conf.mult) sc->int_index = 0;
	timer_reset_set(&sc->heartbeat, sc->intervals[sc->int_index]);
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
	for (u8 i = sc->conf.mult; i < SC_MAXMULT; i++) sc->intervals[i] = sc->intervals[0];
	sc->quarter = sc->intervals[0] >> 2;
	if (!sc->quarter) sc->quarter = 1;
}

static void sc_adjust_position(synced_clock* sc, u32 position) {
    sc->heartbeat.ticksRemain = sc->heartbeat.ticks;

	u32 delta = position;
	u8 expected_index = 0;
	while (delta >= sc->intervals[expected_index] && expected_index < sc->conf.mult) {
		delta -= sc->intervals[expected_index];
		expected_index++;
	}
	if (expected_index == sc->conf.mult) {
		expected_index = 0;
		delta = 0;
	}
	u32 remaining = sc->intervals[expected_index] - delta;
	if (!remaining) remaining = 1;

	u32 since_last = get_ticks() - sc->last_heartbeat;
	if (remaining <= 1) {
		if (since_last < sc->quarter) {
			sc->int_index = (expected_index + 1) % sc->conf.mult;
			sc->heartbeat.ticks = sc->intervals[sc->int_index];
			sc->heartbeat.ticksRemain = sc->intervals[sc->int_index] + remaining;
		} else {
			sc->int_index = expected_index;
			sc->heartbeat.ticksRemain = 1;
		}
	} else if (delta <= 1) {
		if (since_last < sc->quarter) {
			sc->int_index = expected_index;
			sc->heartbeat.ticks = sc->intervals[sc->int_index];
			sc->heartbeat.ticksRemain = remaining;
		} else {
			sc->int_index = (expected_index + sc->conf.mult - 1) % sc->conf.mult;
			sc->heartbeat.ticksRemain = 1;
		}
	} else {
		sc->int_index = expected_index;
		sc->heartbeat.ticks = sc->intervals[sc->int_index];
		sc->heartbeat.ticksRemain = remaining;
	}
}
	
static u32 sc_get_position(synced_clock* sc) {
	u32 position = 0;
	for (u8 i = 0; i < sc->int_index; i++) position += sc->intervals[i];
	position += sc->heartbeat.ticks - sc->heartbeat.ticksRemain;
	return position;
}

static void sc_update_conf(synced_clock* sc) {
    print_dbg("update_conf [\r\n");
  	u32 pos = sc_get_position(sc);
	sc->conf.div = sc->new_conf.div ? sc->new_conf.div : 1;
	sc->conf.mult = sc->new_conf.mult ? sc->new_conf.mult : 1;
	if (sc->conf.mult > SC_MAXMULT) sc->conf.mult = SC_MAXMULT;
    if (sc->int_index >= sc->conf.mult) sc->int_index = 0;
	sc->conf.period = sc->new_conf.period;
	sc->ext_taps_index = sc->ext_taps_count = 0;
	sc->ext_index = sc->int_index = 0;
	sc_recalculate_intervals(sc);
	sc_adjust_position(sc, pos);
    sc->update_conf = 0;
    print_dbg("update_conf ]\r\n");
}

// public methods

void sc_process_tap(synced_clock* sc, u64 tick) {
    print_dbg("process_tap [\r\n");
	u64 elapsed = tick - sc->last_tick;
	sc->last_tick = tick;
    if (sc->in_update) {
        print_dbg("process_tap -\r\n");
        return;
    }
	if (elapsed > (u32)60000) {
		sc->ext_index = sc->ext_taps_index = sc->ext_taps_count = 0;
        print_dbg("process_tap --\r\n");
		return;
	}
    
    sc->in_update = 1;
	if (++sc->ext_index >= sc->conf.div) sc->ext_index = 0;
	sc_update_period(sc, elapsed);
	sc_recalculate_intervals(sc);
	sc_adjust_position(sc, sc->conf.period * sc->ext_index);
    if (sc->update_conf) sc_update_conf(sc);
    sc->in_update = 0;
    print_dbg("process_tap ]\r\n");
}

void sc_update_div(synced_clock* sc, u8 div) {
    sc->update_conf = 1;
    sc->new_conf.div = div;
    sc->new_conf.mult = sc->conf.mult;
    sc->new_conf.period = sc->conf.period;

    if (sc->in_update) return;
    sc->in_update = 1;
    sc_update_conf(sc);
    sc->in_update = 0;
}

void sc_update_mult(synced_clock* sc, u8 mult) {
    sc->update_conf = 1;
    sc->new_conf.div = sc->conf.div;
    sc->new_conf.mult = mult;
    sc->new_conf.period = sc->conf.period;

    if (sc->in_update) return;
    sc->in_update = 1;
    sc_update_conf(sc);
    sc->in_update = 0;
}

void sc_init(synced_clock* sc, sc_config* conf, sc_callback_t callback) {
	sc->conf.div = conf->div ? conf->div : 1;
	sc->conf.mult = conf->mult ? conf->mult : 1;
	sc->conf.period = conf->period;
	sc->callback = callback;

    sc->in_update = 0;
    sc->update_conf = 0;
    
	sc->ext_index = 0;
	sc->int_index = 0;
	sc_recalculate_intervals(sc);
	
	sc->ext_taps_index = 0;
	sc->ext_taps_count = 0;
	sc->last_tick = 0;
	sc->last_heartbeat = 0;

	sc->int_index = sc->conf.mult - 1;
	timer_add(&sc->heartbeat, sc->intervals[sc->int_index], &sc_heartbeat_callback, (void *)sc);
	sc_heartbeat_callback((void *)sc);
}

void sc_load_config(synced_clock* sc, sc_config* conf, u8 from_clock) {
    print_dbg("load_config [\r\n");
    sc->update_conf = 1;
    sc->new_conf.div = conf->div;
    sc->new_conf.mult = conf->mult;
    sc->new_conf.period = conf->period;

    if (sc->in_update) return;
    sc->in_update = 1;
    sc_update_conf(sc);
    sc->in_update = 0;
    print_dbg("load_config ]\r\n");
}

void sc_save_config(synced_clock* sc, sc_config* conf) {
    print_dbg("save_config [\r\n");
	conf->div = sc->conf.div;
	conf->mult = sc->conf.mult;
	conf->period = sc->conf.period;
    print_dbg("save_config ]\r\n");
}