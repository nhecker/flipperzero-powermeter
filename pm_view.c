#include "powermeter.h"

#include <stdio.h>

static void pm_draw_header(Canvas* canvas, PowerMeter* app, const char* title, const char* right) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 8, title);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 128, 8, AlignRight, AlignBottom, right);
    canvas_draw_line(canvas, 0, 10, 127, 10);

    /* Pulse indicator sits just left of the source label. */
    if(furi_get_tick() < app->blink_until) {
        uint16_t w = canvas_string_width(canvas, right);
        canvas_draw_disc(canvas, 123 - w - 4, 4, 2);
    }
}

static void pm_source_label(PowerMeter* app, char* out, size_t len) {
    if(app->cfg.source == PmSourceDemo) {
        snprintf(out, len, "DEMO");
    } else if(app->cfg.source == PmSourceIr) {
        snprintf(out, len, "IR");
    } else if(app->pin_conflict) {
        snprintf(out, len, "%s BUSY", pm_pins[app->cfg.pin_index].label);
    } else {
        snprintf(out, len, "%s", pm_pins[app->cfg.pin_index].label);
    }
}

/* Left column keys share a value offset so the numbers line up; right column
 * values are flushed to the screen edge for the same reason. */
static void pm_draw_pair(Canvas* canvas, int32_t x, int32_t y, const char* key, const char* val) {
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, x, y, key);
    canvas_draw_str(canvas, x + 22, y, val);
}

static void
    pm_draw_pair_r(Canvas* canvas, int32_t x, int32_t y, const char* key, const char* val) {
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, x, y, key);
    canvas_draw_str_aligned(canvas, 127, y, AlignRight, AlignBottom, val);
}

static void pm_draw_live(Canvas* canvas, PowerMeter* app) {
    char buf[48];
    char src[16];

    pm_source_label(app, src, sizeof(src));
    pm_draw_header(canvas, app, "Live", src);

    uint32_t now = pm_instant_watts(app);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)now);
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 104, 33, AlignRight, AlignBottom, buf);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 108, 33, "W");

    /* Same three spans the graph pages plot, so a figure here and the avg on
     * the matching chart always mean the same window. */
    pm_fmt_watts(buf, sizeof(buf), pm_window_watts(app, pm_graph_span(0), NULL));
    pm_draw_pair(canvas, 0, 45, pm_graphs[0].short_title, buf);

    pm_fmt_watts(buf, sizeof(buf), pm_window_watts(app, pm_graph_span(1), NULL));
    pm_draw_pair_r(canvas, 64, 45, pm_graphs[1].short_title, buf);

    pm_fmt_watts(buf, sizeof(buf), pm_window_watts(app, pm_graph_span(2), NULL));
    pm_draw_pair(canvas, 0, 54, pm_graphs[2].short_title, buf);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)app->session_pulses);
    pm_draw_pair_r(canvas, 64, 54, "Pulses", buf);

    char kwh[16];
    char hms[16];
    pm_fmt_kwh(kwh, sizeof(kwh), app->cfg.imp_per_kwh, app->session_pulses);
    pm_fmt_hms(hms, sizeof(hms), (furi_get_tick() - app->start_tick) / 1000);
    snprintf(buf, sizeof(buf), "%s kWh in %s", kwh, hms);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 0, 63, buf);
}

static void pm_draw_graph(Canvas* canvas, PowerMeter* app, uint8_t index) {
    const PmGraphSpec* spec = &pm_graphs[index];
    char buf[24];
    char head[48];

    uint32_t* col = app->graph_col;
    uint32_t peak = 0, low = PM_WATTS_MAX, sum = 0, valid = 0;
    /* Trailing seconds whose energy has not been credited yet are excluded, so
     * the plot and its stats only describe settled data. */
    const uint32_t skip =
        spec->day_ring ? 0 : pm_settled_offset(app, (uint32_t)PM_GRAPH_W * spec->secs_per_px);
    const uint32_t mins = spec->secs_per_px / 60;

    for(uint8_t i = 0; i < PM_GRAPH_W; i++) {
        /* Column 0 is the oldest; the newest bucket sits at the right edge.
         * Columns the history does not fully cover are left empty rather than
         * drawn as a real zero. */
        col[i] = PM_COL_EMPTY;
        uint32_t milli;

        if(spec->day_ring) {
            uint32_t off = (uint32_t)(PM_GRAPH_W - 1 - i) * mins;
            if(off + mins > app->day.filled) continue;
            milli = pm_day_sum_at(&app->day, off, mins);
        } else {
            uint32_t off = (uint32_t)(PM_GRAPH_W - 1 - i) * spec->secs_per_px;
            if(off < skip) continue;
            if(off + spec->secs_per_px > app->ring.filled) continue;
            milli = pm_ring_sum_at(&app->ring, off, spec->secs_per_px);
        }

        col[i] = pm_watts_from_milli(app->cfg.imp_per_kwh, milli, spec->secs_per_px);
        if(col[i] > peak) peak = col[i];
        if(col[i] < low) low = col[i];
        sum += col[i];
        valid++;
    }
    if(!valid) low = 0;
    uint32_t avg = valid ? sum / valid : 0;

    /* Asymmetric latch: adopt a wider range at once, a narrower one only after
     * it has been stable for a while. */
    uint32_t want_lo, want_hi;
    pm_axis_range(low, peak, app->cfg.zero_axis, &want_lo, &want_hi);

    uint32_t now_ms = furi_get_tick();
    bool escaped = !app->axis_held[index] || want_lo < app->axis_lo[index] ||
                   want_hi > app->axis_hi[index];
    if(escaped || (now_ms - app->axis_at[index]) >= PM_AXIS_SETTLE_MS) {
        app->axis_lo[index] = want_lo;
        app->axis_hi[index] = want_hi;
        app->axis_held[index] = true;
        if(!escaped) app->axis_at[index] = now_ms;
    }
    if(escaped) app->axis_at[index] = now_ms;

    const uint32_t axis_lo = app->axis_lo[index];
    const uint32_t axis_hi = app->axis_hi[index];

    if(app->cfg.zero_axis) {
        pm_fmt_watts(buf, sizeof(buf), axis_hi);
    } else {
        pm_fmt_range(buf, sizeof(buf), axis_lo, axis_hi);
    }
    snprintf(head, sizeof(head), "%s%s", app->cfg.log_scale ? "log " : "", buf);
    pm_draw_header(canvas, app, spec->title, head);

    const int32_t base = PM_GRAPH_BOTTOM;
    const int32_t height = PM_GRAPH_BOTTOM - PM_GRAPH_TOP;

    /* Half-scale reference, dotted so it does not read as data. */
    for(int32_t x = PM_GRAPH_X; x < PM_GRAPH_X + PM_GRAPH_W; x += 4) {
        canvas_draw_dot(canvas, x, base - height / 2);
    }
    canvas_draw_line(canvas, PM_GRAPH_X - 1, PM_GRAPH_TOP, PM_GRAPH_X - 1, base);
    canvas_draw_line(canvas, PM_GRAPH_X - 1, base, PM_GRAPH_X + PM_GRAPH_W - 1, base);

    /* A line, not bars: with a fitted axis the baseline is not zero, and a bar
     * whose length no longer encodes magnitude actively misleads. */
    int32_t prev_x = -1, prev_y = 0;
    for(uint8_t i = 0; i < PM_GRAPH_W; i++) {
        if(col[i] == PM_COL_EMPTY) {
            prev_x = -1; /* break the line across gaps rather than bridging */
            continue;
        }
        uint32_t h = pm_bar_height(col[i], axis_lo, axis_hi, (uint32_t)height, app->cfg.log_scale);
        if(h > (uint32_t)height) h = height;

        int32_t x = PM_GRAPH_X + i;
        int32_t y = base - (int32_t)h;
        if(prev_x >= 0) {
            canvas_draw_line(canvas, prev_x, prev_y, x, y);
        } else {
            canvas_draw_dot(canvas, x, y);
        }
        prev_x = x;
        prev_y = y;
    }

    /* min/avg/max of the columns actually plotted, so the numbers always
     * describe this window rather than the whole ring. One string with a
     * single unit: three separately positioned fields could still collide
     * even when their total width fit. */
    char triple[40];
    char labelled[56];
    pm_fmt_triple(triple, sizeof(triple), low, avg, peak);
    snprintf(labelled, sizeof(labelled), "min/avg/max %s", triple);

    canvas_set_font(canvas, FontSecondary);
    const char* stats = canvas_string_width(canvas, labelled) <= 126 ? labelled : triple;
    canvas_draw_str(canvas, 0, 63, stats);
}

static void pm_draw_row(Canvas* canvas, int32_t y, const char* key, const char* val) {
    canvas_draw_str(canvas, 0, y, key);
    canvas_draw_str(canvas, 54, y, val);
}

/* Bring-up aid: raw counters, so a source that produces nothing can be told
 * apart from one whose output is being filtered away. */
static void pm_draw_diag(Canvas* canvas, PowerMeter* app) {
    char src[16];
    char buf[24];

    pm_source_label(app, src, sizeof(src));
    pm_draw_header(canvas, app, "Diag", src);

    FURI_CRITICAL_ENTER();
    uint32_t rejected = app->cap.rejected;
    uint32_t ir_edges = app->cap.ir_edges;
    uint32_t ir_last = app->cap.ir_last_us;
    FURI_CRITICAL_EXIT();

    canvas_set_font(canvas, FontSecondary);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)app->session_pulses);
    pm_draw_row(canvas, 20, "pulses", buf);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)rejected);
    pm_draw_row(canvas, 28, "rejected", buf);

    if(app->have_pulse) {
        snprintf(buf, sizeof(buf), "%lums", (unsigned long)app->last_interval);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    pm_draw_row(canvas, 36, "interval", buf);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)ir_edges);
    pm_draw_row(canvas, 44, "IR edges", buf);

    snprintf(buf, sizeof(buf), "%luus", (unsigned long)ir_last);
    pm_draw_row(canvas, 52, "IR mark", buf);

    if(app->cfg.source == PmSourceIr) {
        snprintf(buf, sizeof(buf), "IR");
    } else if(app->cfg.source == PmSourceDemo) {
        snprintf(buf, sizeof(buf), "Demo %luW", (unsigned long)app->cfg.demo_watts);
    } else if(app->pin_conflict) {
        snprintf(buf, sizeof(buf), "%s busy", pm_pins[app->cfg.pin_index].label);
    } else if(app->gpio_armed) {
        snprintf(
            buf,
            sizeof(buf),
            "%s %s",
            pm_pins[app->cfg.pin_index].label,
            furi_hal_gpio_read(app->armed_pin) ? "HIGH" : "LOW");
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    pm_draw_row(canvas, 60, "source", buf);
}

void pm_view_draw(Canvas* canvas, void* model) {
    PmModel* m = model;
    PowerMeter* app = m->app;

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    if(app->page == PmPageLive) {
        pm_draw_live(canvas, app);
    } else if(app->page == PmPageDiag) {
        pm_draw_diag(canvas, app);
    } else {
        pm_draw_graph(canvas, app, (uint8_t)(app->page - PmPageGraphShort));
    }
}

/* Commit the (unchanged) model purely to schedule a repaint. */
void pm_view_refresh(PowerMeter* app) {
    with_view_model(app->main_view, PmModel * model, { UNUSED(model); }, true);
}

bool pm_view_input(InputEvent* event, void* ctx) {
    PowerMeter* app = ctx;

    if(event->type != InputTypeShort && event->type != InputTypeRepeat &&
       event->type != InputTypeLong) {
        return false;
    }

    switch(event->key) {
    case InputKeyLeft:
        app->page = (app->page + PmPageCount - 1) % PmPageCount;
        pm_view_refresh(app);
        return true;
    case InputKeyRight:
        app->page = (app->page + 1) % PmPageCount;
        pm_view_refresh(app);
        return true;
    case InputKeyOk:
        if(event->type == InputTypeLong) {
            pm_session_reset(app);
            notification_message(app->notifications, &sequence_success);
        } else if(event->type == InputTypeShort) {
            view_dispatcher_send_custom_event(app->view_dispatcher, PmEventSettings);
        }
        return true;
    case InputKeyBack:
        if(event->type == InputTypeShort) {
            view_dispatcher_send_custom_event(app->view_dispatcher, PmEventExit);
        }
        return true;
    default:
        return false;
    }
}
