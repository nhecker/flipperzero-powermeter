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
    } else {
        snprintf(out, len, "%s", pm_pins[app->cfg.pin_index].label);
    }
}

static void pm_draw_pair(Canvas* canvas, int32_t x, int32_t y, const char* key, const char* val) {
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, x, y, key);
    canvas_draw_str(canvas, x + 22, y, val);
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

    bool partial = false;
    pm_fmt_watts(buf, sizeof(buf), pm_window_watts(app, 60, &partial));
    pm_draw_pair(canvas, 0, 45, partial ? "1m~" : "1m", buf);

    pm_fmt_watts(buf, sizeof(buf), pm_window_watts(app, 900, &partial));
    pm_draw_pair(canvas, 64, 45, partial ? "15m~" : "15m", buf);

    pm_fmt_watts(buf, sizeof(buf), pm_window_watts(app, 3600, &partial));
    pm_draw_pair(canvas, 0, 54, partial ? "60m~" : "60m", buf);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)app->session_pulses);
    pm_draw_pair(canvas, 64, 54, "n", buf);

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
    uint32_t peak = 0;
    for(uint8_t i = 0; i < PM_GRAPH_W; i++) {
        /* Column 0 is the oldest; the newest bucket sits at the right edge. */
        uint32_t offset = (uint32_t)(PM_GRAPH_W - 1 - i) * spec->secs_per_px;
        uint32_t pulses = pm_ring_sum_at(&app->ring, offset, spec->secs_per_px);
        col[i] = pm_watts_from_pulses(app->cfg.imp_per_kwh, pulses, spec->secs_per_px);
        if(col[i] > peak) peak = col[i];
    }

    uint32_t scale = pm_nice_ceiling(peak < 100 ? 100 : peak);
    pm_fmt_watts(buf, sizeof(buf), scale);
    snprintf(head, sizeof(head), "max %s", buf);
    pm_draw_header(canvas, app, spec->title, head);

    const int32_t base = PM_GRAPH_BOTTOM;
    const int32_t height = PM_GRAPH_BOTTOM - PM_GRAPH_TOP;

    /* Half-scale reference, dotted so it does not read as data. */
    for(int32_t x = PM_GRAPH_X; x < PM_GRAPH_X + PM_GRAPH_W; x += 4) {
        canvas_draw_dot(canvas, x, base - height / 2);
    }
    canvas_draw_line(canvas, PM_GRAPH_X - 1, PM_GRAPH_TOP, PM_GRAPH_X - 1, base);
    canvas_draw_line(canvas, PM_GRAPH_X - 1, base, PM_GRAPH_X + PM_GRAPH_W - 1, base);

    for(uint8_t i = 0; i < PM_GRAPH_W; i++) {
        if(col[i] == 0) continue;
        uint32_t h = (uint32_t)(((uint64_t)col[i] * height) / scale);
        if(h == 0) h = 1;
        if(h > (uint32_t)height) h = height;
        canvas_draw_line(canvas, PM_GRAPH_X + i, base - h, PM_GRAPH_X + i, base - 1);
    }

    canvas_set_font(canvas, FontSecondary);
    pm_fmt_watts(buf, sizeof(buf), pm_instant_watts(app));
    snprintf(head, sizeof(head), "now %s", buf);
    canvas_draw_str(canvas, 0, 63, head);

    uint32_t span = (uint32_t)PM_GRAPH_W * spec->secs_per_px;
    pm_fmt_watts(buf, sizeof(buf), pm_window_watts(app, span, NULL));
    snprintf(head, sizeof(head), "avg %s", buf);
    canvas_draw_str_aligned(canvas, 128, 63, AlignRight, AlignBottom, head);
}

void pm_view_draw(Canvas* canvas, void* model) {
    PmModel* m = model;
    PowerMeter* app = m->app;

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    if(app->page == PmPageLive) {
        pm_draw_live(canvas, app);
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
