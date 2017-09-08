#include "print_funcs.h"
#include "synced_clock.h"
#include "compiler.h"
#include "init_trilogy.h"
#include "interrupts.h"

// lowest common denominator for 1..16
#define SC_PHASE_DENOM (u32)720720
#define DEBUG 0

const u32 sc_phase_fractions[SC_MAXMULT] = {720720, 360360, 240240, 180180, 144144,
    120120, 102960, 90090, 80080, 72072, 65520, 60060, 55440, 51480, 48048, 45045};
    
// private methods

static void debug(synced_clock* sc) {
    print_dbg("\r\n[");
    print_dbg_ulong(sc->period);
    print_dbg(" ");
    print_dbg_ulong(sc->div);
    print_dbg(" ");
    print_dbg_ulong(sc->mult);
    print_dbg(" ");
    print_dbg_ulong(sc->phase_lock);
    print_dbg("]");
}

static u32 sc_get_position(synced_clock* sc, u8 is_tap);
static void sc_calculate_intervals(synced_clock* sc);
static void sc_adjust_position(synced_clock* sc, u32 position);
static void sc_changes_from_clock_callback(void* o);
static void sc_changes_from_config_callback(void* o);

static void sc_reset_taps(sc_taps* taps) {
    taps->count = taps->index = 0;
}

static void sc_update_taps(sc_taps* taps, u32 tap) {
    if (tap < taps->last_tap - taps->quarter || tap > taps->last_tap + taps->quarter) sc_reset_taps(taps);
    taps->taps[taps->index] = taps->last_tap = tap;
    taps->quarter = tap >> 2; if (!taps->quarter) taps->quarter = 1;
    if (++taps->index >= SC_AVERAGING_TAPS) taps->index = 0;
    if (taps->count < SC_AVERAGING_TAPS) taps->count++;
}

static u32 sc_get_period(sc_taps* taps) {
    u64 total = 0;
    for (u8 i = 0; i < taps->count; i++) total += taps->taps[i];
    return total / (u64)taps->count;
}

static void sc_heartbeat_callback(void* o) {
    synced_clock* sc = o;
    if ((get_ticks() - sc->clock.last_heartbeat) < sc->clock.quarter) return;

    sc->clock.last_heartbeat = get_ticks();
    if (DEBUG) print_dbg("^");
    
    if (++sc->clock.interval >= sc->mult) sc->clock.interval = 0;
    timer_reset_set(&sc->clock.heartbeat, sc->clock.intervals[sc->clock.interval]);
    (*(sc->clock.callback))();
}

static void sc_changes_from_first_tap_callback(void* o) {
    synced_clock* sc = o;
    timer_remove(&sc->changes_from_first_tap);

    u32 position = sc_get_position(sc, 1);
    sc_calculate_intervals(sc);
    sc_adjust_position(sc, position);
    if (DEBUG) { debug(sc); print_dbg("'"); }
}

static void sc_changes_from_clock_callback(void* o) {
    synced_clock* sc = o;
    timer_remove(&sc->changes_from_clock);
    
    sc->period = sc->new_period;
    u32 position = sc_get_position(sc, 1);
    sc_calculate_intervals(sc);
    sc_adjust_position(sc, position);
    if (DEBUG) { debug(sc); print_dbg("*"); }
}

static void sc_changes_from_config_callback(void* o) {
    synced_clock* sc = o;
    timer_remove(&sc->changes_from_config);
    
    sc->div = sc->new_div;
    sc->mult = sc->new_mult;
    u32 position = sc_get_position(sc, 0);
    sc_calculate_intervals(sc);
    sc_adjust_position(sc, position);
    if (DEBUG) { debug(sc); print_dbg("."); }
}

static u32 sc_get_position(synced_clock* sc, u8 is_tap) {
    if (DEBUG) print_dbg("\r\nget_position ");
    u32 position = 0;
     
    if (is_tap) {
        u32 full_period = sc->period * sc->div;
        position = sc->period * (u32)sc->div_index + get_ticks() - sc->taps.last_tick;
        if (!sc->phase_lock)
            position = (position + full_period - (full_period * sc->phase_offset) / SC_PHASE_DENOM) % full_period;
    } else {
        for (u8 i = 0; i < sc->clock.interval; i++) position += sc->clock.intervals[i];
        position += sc->clock.heartbeat.ticks - sc->clock.heartbeat.ticksRemain;        
    }
     
    return position;
}

static void sc_calculate_intervals(synced_clock* sc) {
    u32 total = sc->period * sc->div;
    u32 interval = (total << 4) / (u32)sc->mult;
    u32 acc = 0;
    u32 carryover = 0;
    for (u8 i = 0; i < sc->mult; i++) {
        sc->clock.intervals[i] = (interval + carryover) >> 4;
        carryover = (interval + carryover) & 15;
        acc += sc->clock.intervals[i];
    }
    sc->clock.intervals[sc->mult - 1] += total - acc;
    for (u8 i = sc->mult; i < SC_MAXMULT; i++) sc->clock.intervals[i] = sc->clock.intervals[0];
    sc->clock.quarter = sc->clock.intervals[0] >> 2;
    if (!sc->clock.quarter) sc->clock.quarter = 1;
}

static void sc_adjust_position(synced_clock* sc, u32 position) {
    if (DEBUG) { print_dbg("\r\nadjust_phase pos:"); print_dbg_ulong(position); }

    u32 delta = position;
    u8 interval = 0;
    while (delta >= sc->clock.intervals[interval] && interval < sc->mult) {
        delta -= sc->clock.intervals[interval];
        interval++;
    }
    if (interval == sc->mult) {
        interval = 0;
        delta = 0;
    }
    
    u32 remaining = sc->clock.intervals[interval] - delta;
    if (!remaining) remaining = 1;

    if (DEBUG) {
        print_dbg(" delta:");
        print_dbg_ulong(delta);
        print_dbg(" int:");
        print_dbg_ulong(interval);
    }
    
    if (sc->clock.heartbeat.ticksRemain < 3 && delta < 3) {
        sc->clock.interval = (interval + sc->mult - 1) % sc->mult;
        sc->clock.heartbeat.ticks = sc->clock.intervals[sc->clock.interval];
        sc->clock.heartbeat.ticksRemain = 1;
    } else {
        sc->clock.interval = interval;
        sc->clock.heartbeat.ticks = sc->clock.intervals[sc->clock.interval];
        sc->clock.heartbeat.ticksRemain = remaining;
    }
}

// public methods

void sc_init(synced_clock* sc, u8 div, u8 mult, u32 period, u8 phase_lock, sc_callback_t callback) {
    sc->period = period;
    sc->div = div;
    sc->mult = mult;
    sc->phase_lock = phase_lock;

    sc->clock.callback = callback;
    sc->clock.interval = sc->mult - 1;
    sc->clock.last_heartbeat = 0;

    sc_reset_taps(&sc->taps);
    sc->taps.last_tap = 0;
    sc->taps.last_tick = 0;

    sc_calculate_intervals(sc);
    
    sc->div_index = 0;
    sc->phase_offset = 0;
}

void sc_start(synced_clock* sc) {
    if (DEBUG) print_dbg("\r\nstart");
    
    sc->div_index = 0;
    sc->phase_offset = 0;

    sc->clock.interval = sc->mult - 1;
    timer_add(&sc->clock.heartbeat, 1, sc_heartbeat_callback, (void *)sc);
}

void sc_stop(synced_clock* sc) {
    if (DEBUG) print_dbg("\r\nstop");
    timer_remove(&sc->clock.heartbeat);
}

u32 sc_process_clock(synced_clock* sc, u8 use_average) {
    u64 tick = get_ticks();
    u64 tap = tick - sc->taps.last_tick;
    sc->taps.last_tick = tick;
    if (DEBUG) {
        print_dbg("\r\nprocess_clock lt:"); 
        print_dbg_ulong(sc->taps.last_tick);
        print_dbg(" tap:"); 
        print_dbg_ulong(tap);
    }

    if (tap > (sc->taps.last_tap << 2)) {
        sc->div_index = 0;
        sc->phase_offset = 0;
        sc_update_taps(&sc->taps, tap);
        timer_remove(&sc->changes_from_first_tap);
        timer_add(&sc->changes_from_first_tap, 1, &sc_changes_from_first_tap_callback, (void *)sc);
        if (DEBUG) print_dbg(" ft");
        return sc->period;
    }

    if (++sc->div_index >= sc->div) sc->div_index = 0;
    if (tap < sc->taps.last_tap - sc->taps.quarter || tap > sc->taps.last_tap + sc->taps.quarter) {
        sc->phase_offset = 0;
        print_dbg("\r\nWTF ");
        print_dbg_ulong(sc->taps.last_tap);
        print_dbg(" ");
        print_dbg_ulong(sc->taps.quarter);
        print_dbg(" ");
        print_dbg_ulong(tap);
    }
    
    if (!use_average) sc_reset_taps(&sc->taps);
    sc_update_taps(&sc->taps, tap);
    
    sc->new_period = sc_get_period(&sc->taps);
    if (DEBUG) { print_dbg(" new: "); print_dbg_ulong(sc->new_period); }
    timer_remove(&sc->changes_from_clock);
    timer_add(&sc->changes_from_clock, 1, &sc_changes_from_clock_callback, (void *)sc);    
    return sc->new_period;
}

void sc_update_divmult(synced_clock* sc, u8 div, u8 mult, u8 from_timer) {
    if (DEBUG) print_dbg("\r\nupdate_config ");
    
    if (from_timer) {
        if (DEBUG) { print_dbg("imm "); print_dbg_ulong(sc->phase_offset); }
        u32 pos = sc_get_position(sc, 0);
        if (!sc->phase_lock) {
            sc->phase_offset = (sc->phase_offset + sc->clock.interval * sc_phase_fractions[sc->mult - 1]) % SC_PHASE_DENOM;
            pos = 0;
        }
        sc->div = div;
        sc->mult = mult;
        sc_calculate_intervals(sc);
        sc_adjust_position(sc, pos);
    } else {
        sc->new_div = div;
        sc->new_mult = mult;
        timer_remove(&sc->changes_from_config);
        timer_add(&sc->changes_from_config, 1, &sc_changes_from_config_callback, (void *)sc);    
    }
}

void sc_set_lock_mode(synced_clock* sc, u8 phase_lock) {
    if (DEBUG) { print_dbg("\r\nset_lock_mode "); print_dbg_ulong(phase_lock); }
    sc->phase_lock = phase_lock;
    if (phase_lock) sc->phase_offset = 0;
}
