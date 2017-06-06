#include "synced_clock.h"
#include "compiler.h"
#include "init_trilogy.h"
#include "interrupts.h"

static void sc_heartbeat_callback(void* o);
static void sc_update_period(synced_clock* sc, u32 tap);
static void sc_recalculate_intervals(synced_clock* sc);
static void sc_adjust_position(synced_clock* sc, u32 position);
static u32 sc_get_position(synced_clock* sc);

static void sc_heartbeat_callback(void* o) {
	synced_clock* sc = o;
	
	u8 irq_flags = irqs_pause();
	
	if ((get_ticks() - sc->last_heartbeat) < sc->quarter) {
		sc_debug2 = get_ticks() - sc->last_heartbeat;
		sc_debug1 = 0b111111111111;
		irqs_resume(irq_flags);
		return;
	}
	
	sc->last_heartbeat = get_ticks();
	if (++sc->int_index >= sc->conf.mult) sc->int_index = 0;
	timer_reset_set(&sc->heartbeat, sc->intervals[sc->int_index]);
	
	irqs_resume(irq_flags);

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

	u8 irq_flags = irqs_pause();
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
	irqs_resume(irq_flags);
	
	(*(sc_notify))();
}
	
static u32 sc_get_position(synced_clock* sc) {
	u32 position = 0;
	u8 irq_flags = irqs_pause();
	for (u8 i = 0; i < sc->int_index; i++) position += sc->intervals[i];
	position += sc->heartbeat.ticks - sc->heartbeat.ticksRemain;
	irqs_resume(irq_flags);
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
	if (sc->conf.mult > SC_MAXMULT) sc->conf.mult = SC_MAXMULT;
	if (sc->int_index >= sc->conf.mult) sc->int_index = 0;
	sc_recalculate_intervals(sc);
	sc_adjust_position(sc, pos);
}

void sc_init(synced_clock* sc, sc_config* conf, sc_callback_t callback) {
	sc_debug1 = sc_debug2 = 0;
	
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
	sc->last_heartbeat = 0;

	sc->int_index = sc->conf.mult - 1;
	timer_add(&sc->heartbeat, sc->intervals[sc->int_index], &sc_heartbeat_callback, (void *)sc);
	sc_heartbeat_callback((void *)sc);
}

void sc_load_config(synced_clock* sc, sc_config* conf, u8 update_period, u8 update_div_mult, u8 from_clock) {
	sc_debug1 = sc_debug2 = 0;

	u32 pos = from_clock ? 0 : sc_get_position(sc);
	if (update_div_mult) {
		sc->conf.div = conf->div ? conf->div : 1;
		sc->conf.mult = conf->mult ? conf->mult : 1;
		if (sc->conf.mult > SC_MAXMULT) sc->conf.mult = SC_MAXMULT;
	}
	if (update_period) {
		sc->conf.period = conf->period;
		sc->ext_taps_index = sc->ext_taps_count = 0;
	}
	if (update_div_mult || update_period) {
		sc->ext_index = sc->int_index = 0;
		sc_recalculate_intervals(sc);
		if (from_clock)
			timer_reset_set(&sc->heartbeat, sc->intervals[sc->int_index]);
		else
			sc_adjust_position(sc, pos);
	}
}

void sc_save_config(synced_clock* sc, sc_config* conf) {
	conf->div = sc->conf.div;
	conf->mult = sc->conf.mult;
	conf->period = sc->conf.period;
}
