#include "powermeter.h"

const PmPinDef pm_pins[] = {
    {"PC3 (7)", &gpio_ext_pc3},
    {"PA7 (2)", &gpio_ext_pa7},
    {"PA6 (3)", &gpio_ext_pa6},
    {"PA4 (4)", &gpio_ext_pa4},
    {"PB3 (6)", &gpio_ext_pb3},
    {"PB2 (5)", &gpio_ext_pb2},
    {"PC1 (15)", &gpio_ext_pc1},
    {"PC0 (16)", &gpio_ext_pc0},
};
const size_t pm_pin_count = COUNT_OF(pm_pins);

const PmGraphSpec pm_graphs[] = {
    {"2 min", 1},
    {"30 min", 15},
    {"60 min", 30},
};

static const NotificationSequence pm_seq_beep = {
    &message_note_c5,
    &message_delay_10,
    &message_sound_off,
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

void pm_capture_start(PowerMeter* app) {
    if(app->gpio_armed || app->cfg.source != PmSourceGpio) return;
    if(app->cfg.pin_index >= pm_pin_count) app->cfg.pin_index = 0;

    app->armed_pin = pm_pins[app->cfg.pin_index].pin;
    furi_hal_gpio_init(
        app->armed_pin, GpioModeInterruptRiseFall, (GpioPull)app->cfg.pull, GpioSpeedVeryHigh);
    furi_hal_gpio_add_int_callback(app->armed_pin, pm_gpio_isr, app);
    furi_hal_gpio_enable_int_callback(app->armed_pin);
    app->gpio_armed = true;
}

void pm_capture_stop(PowerMeter* app) {
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
    if(app->cfg.led_feedback) notification_message(app->notifications, &sequence_blink_green_10);
    if(app->cfg.beep_feedback) notification_message(app->notifications, &pm_seq_beep);
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

    pm_ring_advance(&app->ring, now / 1000);
    if(fresh) {
        pm_ring_add(&app->ring, fresh);
        app->session_pulses += fresh;
        app->blink_until = now + PM_BLINK_MS;
        pm_feedback(app);
    }

    pm_view_refresh(app);
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
    uint32_t pulses = pm_ring_sum(&app->ring, span_sec, &used);
    if(partial) *partial = used < span_sec;
    return pm_watts_from_pulses(app->cfg.imp_per_kwh, pulses, used);
}
