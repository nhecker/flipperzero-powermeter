#include "powermeter.h"

/* EXTI callback slots are indexed by pin NUMBER, not by port, so PxN and PyN
 * share one slot across ports. furi_hal_gpio_add_int_callback asserts the slot
 * is free, so claiming a line the firmware already owns resets the device
 * rather than failing softly -- the OK button alone (PH3, line 3) rules out
 * both PC3 and PB3.
 *
 * Rather than hardcode a blocklist that would rot across firmware versions,
 * pm_exti_snapshot() reads which lines are already unmasked and the UI marks
 * those pins busy. Every ext pin stays listed; the unusable ones say so. */
const PmPinDef pm_pins[] = {
    {"PA4", &gpio_ext_pa4},
    {"PA6", &gpio_ext_pa6},
    {"PA7", &gpio_ext_pa7},
    {"PB2", &gpio_ext_pb2},
    {"PB3", &gpio_ext_pb3},
    {"PC0", &gpio_ext_pc0},
    {"PC1", &gpio_ext_pc1},
    {"PC3", &gpio_ext_pc3},
};
const size_t pm_pin_count = COUNT_OF(pm_pins);

const PmGraphSpec pm_graphs[] = {
    {"2 min", 1},
    {"30 min", 15},
    {"60 min", 30},
};

/* One blip per pulse. Every element is gated by the user's own system settings
 * -- LED brightness, speaker volume, vibro enable -- so there is nothing here
 * for the app to duplicate as its own toggles. */
static const NotificationSequence pm_seq_pulse = {
    &message_green_255,
    &message_vibro_on,
    &message_note_c5,
    &message_delay_10,
    &message_sound_off,
    &message_vibro_off,
    &message_green_0,
    NULL,
};

/* Timestamp on the leading edge, commit on the trailing one: the width check
 * throws out mains-frequency flicker and contact chatter before it reaches the
 * statistics. */
static void pm_gpio_isr(void* ctx) {
    PowerMeter* app = ctx;
    PmCapture* c = &app->cap;
    uint32_t now = furi_get_tick();
    bool active = furi_hal_gpio_read(app->armed_pin) == app->cfg.active_high;

    if(active) {
        c->edge_tick = now;
        c->edge_pending = true;
        return;
    }
    if(!c->edge_pending) return;
    c->edge_pending = false;

    uint32_t width = now - c->edge_tick;
    if(width < app->cfg.min_pulse_ms || width > app->cfg.max_pulse_ms) {
        c->rejected++;
        return;
    }
    if(c->count) c->last_interval = c->edge_tick - c->last_tick;
    c->last_tick = c->edge_tick;
    c->count++;
}

/* The onboard receiver is a 38 kHz demodulator, so an unmodulated meter LED
 * produces at most an edge transient rather than a clean mark. Count one pulse
 * per burst of activity and use max_pulse_ms as the refractory window; the raw
 * edge counter on the diagnostics page shows what the TSOP actually saw. */
static void pm_ir_isr(void* ctx, bool level, uint32_t duration) {
    PowerMeter* app = ctx;
    PmCapture* c = &app->cap;

    c->ir_edges++;
    if(!level) return;
    c->ir_last_us = duration;

    uint32_t now = furi_get_tick();
    if(c->count && (now - c->last_tick) < app->cfg.max_pulse_ms) {
        c->rejected++;
        return;
    }
    if(c->count) c->last_interval = now - c->last_tick;
    c->last_tick = now;
    c->count++;
}

static void pm_ir_timeout_isr(void* ctx) {
    UNUSED(ctx);
}

uint8_t pm_pin_line(uint8_t index) {
    if(index >= pm_pin_count) return 0;
    return (uint8_t)__builtin_ctz(pm_pins[index].pin->pin);
}

/* Must run before we arm anything, or our own line reads back as taken. */
void pm_exti_snapshot(PowerMeter* app) {
    app->exti_taken = 0;
    for(uint8_t line = 0; line < 16; line++) {
        if(LL_EXTI_IsEnabledIT_0_31(1UL << line)) app->exti_taken |= 1UL << line;
    }
}

bool pm_pin_available(const PowerMeter* app, uint8_t index) {
    if(index >= pm_pin_count) return false;
    return (app->exti_taken & (1UL << pm_pin_line(index))) == 0;
}

void pm_capture_start(PowerMeter* app) {
    if(app->cfg.source == PmSourceIr) {
        if(app->ir_armed || furi_hal_infrared_is_busy()) return;
        furi_hal_infrared_async_rx_set_capture_isr_callback(pm_ir_isr, app);
        furi_hal_infrared_async_rx_set_timeout_isr_callback(pm_ir_timeout_isr, app);
        furi_hal_infrared_async_rx_start();
        furi_hal_infrared_async_rx_set_timeout(PM_IR_TIMEOUT_US);
        app->ir_armed = true;
        return;
    }

    if(app->gpio_armed || app->cfg.source != PmSourceGpio) return;
    if(app->cfg.pin_index >= pm_pin_count) app->cfg.pin_index = 0;

    /* Arming a line the firmware owns would trip a furi_check and reset the
     * device, so refuse and let the UI say why. */
    if(!pm_pin_available(app, app->cfg.pin_index)) {
        app->pin_conflict = true;
        return;
    }
    app->pin_conflict = false;

    app->armed_pin = pm_pins[app->cfg.pin_index].pin;
    furi_hal_gpio_init(
        app->armed_pin, GpioModeInterruptRiseFall, (GpioPull)app->cfg.pull, GpioSpeedVeryHigh);
    furi_hal_gpio_add_int_callback(app->armed_pin, pm_gpio_isr, app);
    furi_hal_gpio_enable_int_callback(app->armed_pin);
    app->gpio_armed = true;
}

void pm_capture_stop(PowerMeter* app) {
    if(app->ir_armed) {
        furi_hal_infrared_async_rx_stop();
        app->ir_armed = false;
    }
    if(!app->gpio_armed) return;
    furi_hal_gpio_disable_int_callback(app->armed_pin);
    furi_hal_gpio_remove_int_callback(app->armed_pin);
    furi_hal_gpio_init(app->armed_pin, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    app->gpio_armed = false;
    app->armed_pin = NULL;
}

void pm_capture_restart(PowerMeter* app) {
    pm_capture_stop(app);
    pm_capture_start(app);
}

void pm_session_reset(PowerMeter* app) {
    FURI_CRITICAL_ENTER();
    app->cap.count = 0;
    app->cap.rejected = 0;
    app->cap.last_tick = 0;
    app->cap.last_interval = 0;
    app->cap.edge_pending = false;
    app->cap.ir_edges = 0;
    app->cap.ir_last_us = 0;
    FURI_CRITICAL_EXIT();

    pm_ring_reset(&app->ring);
    app->seen_count = 0;
    app->session_pulses = 0;
    app->last_interval = 0;
    app->last_pulse_tick = 0;
    app->have_pulse = false;
    app->demo_accum = 0;
    app->start_tick = furi_get_tick();
}

static uint32_t pm_rand(PowerMeter* app) {
    uint32_t x = app->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    app->rng = x;
    return x;
}

/* Synthesise pulses at the rate the configured demo load implies, with a little
 * jitter so the graphs do not look like a test pattern. */
static uint32_t pm_demo_pulses(PowerMeter* app) {
    if(app->cfg.demo_watts == 0) return 0;
    uint32_t watts = app->cfg.demo_watts;
    uint32_t jitter = watts / 8;
    if(jitter) watts = watts - jitter + (pm_rand(app) % (2 * jitter + 1));

    /* micro-pulses this tick = W * imp * tick_ms / 3600 */
    app->demo_accum += (uint32_t)(((uint64_t)watts * app->cfg.imp_per_kwh * PM_TICK_MS) / 3600ULL);
    uint32_t pulses = app->demo_accum / 1000000u;
    app->demo_accum -= pulses * 1000000u;
    return pulses;
}

static void pm_feedback(PowerMeter* app) {
    notification_message(app->notifications, &pm_seq_pulse);
}

void pm_tick(void* ctx) {
    PowerMeter* app = ctx;
    uint32_t now = furi_get_tick();
    uint32_t fresh = 0;

    if(app->cfg.source == PmSourceDemo) {
        fresh = pm_demo_pulses(app);
        if(fresh) {
            uint32_t gap = app->have_pulse ? (now - app->last_pulse_tick) : PM_TICK_MS;
            app->last_interval = gap / fresh;
            if(app->last_interval == 0) app->last_interval = 1;
            app->last_pulse_tick = now;
            app->have_pulse = true;
        }
    } else {
        FURI_CRITICAL_ENTER();
        uint32_t count = app->cap.count;
        uint32_t last_tick = app->cap.last_tick;
        uint32_t interval = app->cap.last_interval;
        FURI_CRITICAL_EXIT();

        fresh = count - app->seen_count;
        app->seen_count = count;
        if(fresh) {
            app->last_pulse_tick = last_tick;
            app->have_pulse = true;
            if(interval) app->last_interval = interval;
        }
    }

    uint32_t now_sec = now / 1000;
    bool second_rolled = now_sec != app->last_draw_sec;
    pm_ring_advance(&app->ring, now_sec);

    if(fresh) {
        /* Attribute the energy to the interval it flowed over, not to the
         * instant the pulse landed, so a slow meter reads as a level load
         * instead of a comb of spikes. */
        pm_ring_add_interval(&app->ring, fresh * PM_MILLI, app->last_interval, now % 1000);
        app->session_pulses += fresh;
        app->blink_until = now + PM_BLINK_MS;
        pm_feedback(app);
    }

    /* Nothing on screen changes faster than once a second except the pulse
     * blip, so there is no reason to repaint at tick rate. */
    if(fresh || second_rolled) {
        app->last_draw_sec = now_sec;
        pm_view_refresh(app);
    }
}

/* Fall back to the elapsed-since-last-pulse interval once it exceeds the last
 * measured one, so a load dropping to zero decays instead of freezing. */
uint32_t pm_instant_watts(const PowerMeter* app) {
    if(!app->have_pulse || app->last_interval == 0) return 0;
    uint32_t elapsed = furi_get_tick() - app->last_pulse_tick;
    uint32_t interval = elapsed > app->last_interval ? elapsed : app->last_interval;
    return pm_watts_from_interval(app->cfg.imp_per_kwh, interval);
}

uint32_t pm_window_watts(const PowerMeter* app, uint32_t span_sec, bool* partial) {
    uint32_t used = 0;
    uint32_t milli = pm_ring_sum(&app->ring, span_sec, &used);
    if(partial) *partial = used < span_sec;
    return pm_watts_from_milli(app->cfg.imp_per_kwh, milli, used);
}
