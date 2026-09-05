#pragma once

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_infrared.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/variable_item_list.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#include "pm_calc.h"

#define PM_TICK_MS       100u
#define PM_BLINK_MS      180u
#define PM_IR_TIMEOUT_US 20000u
#define PM_GRAPH_X       4
#define PM_GRAPH_W       120
#define PM_GRAPH_TOP     13
#define PM_GRAPH_BOTTOM  53

#define PM_CONFIG_PATH    APP_DATA_PATH("powermeter.conf")
#define PM_CONFIG_HEADER  "PowerMeter config"
#define PM_CONFIG_VERSION 2

typedef enum {
    PmViewMain,
    PmViewSettings,
} PmViewId;

typedef enum {
    PmEventExit = 100,
    PmEventSettings,
} PmEvent;

typedef enum {
    PmPageLive,
    PmPageGraphShort,
    PmPageGraphMid,
    PmPageGraphLong,
    PmPageDiag,
    PmPageCount,
} PmPage;

typedef enum {
    PmSourceGpio,
    PmSourceIr,
    PmSourceDemo,
    PmSourceCount,
} PmSource;

typedef struct {
    const char* label;
    const GpioPin* pin;
} PmPinDef;

typedef struct {
    const char* title;
    uint32_t secs_per_px;
} PmGraphSpec;

extern const PmPinDef pm_pins[];
extern const size_t pm_pin_count;
extern const PmGraphSpec pm_graphs[];

typedef struct {
    uint32_t imp_per_kwh;
    uint8_t pin_index;
    uint8_t pull;
    bool active_high;
    uint16_t min_pulse_ms;
    uint16_t max_pulse_ms;
    uint8_t source;
    uint32_t demo_watts;
} PmConfig;

/* Written by the GPIO ISR, drained under a critical section by the tick. */
typedef struct {
    volatile uint32_t count;
    volatile uint32_t rejected;
    volatile uint32_t last_tick;
    volatile uint32_t last_interval;
    volatile uint32_t edge_tick;
    volatile bool edge_pending;
    /* Bring-up scaffolding (see Diag page): raw receiver edges and the last
     * mark length, so IR mode can distinguish "TSOP saw nothing" from "TSOP
     * saw chatter and the refractory window ate it". Strip before v1.0. */
    volatile uint32_t ir_edges;
    volatile uint32_t ir_last_us;
} PmCapture;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* main_view;
    VariableItemList* settings_list;
    NotificationApp* notifications;
    Storage* storage;

    PmConfig cfg;
    PmCapture cap;
    PmRing ring;

    PmPage page;
    bool gpio_armed;
    bool ir_armed;
    bool pin_conflict;
    /* EXTI interrupt-mask bits already set when we started, i.e. lines the
     * firmware owns. Sampled before we arm anything so our own line never
     * shows up here. */
    uint32_t exti_taken;
    const GpioPin* armed_pin;

    uint32_t start_tick;
    uint32_t session_pulses;
    uint32_t seen_count;
    uint32_t last_interval;
    uint32_t last_pulse_tick;
    bool have_pulse;
    uint32_t blink_until;
    uint32_t last_draw_sec;

    uint32_t demo_accum;
    uint32_t rng;

    /* Scratch for graph rendering. Lives here rather than on the draw
     * callback's stack, which belongs to the GUI service thread. */
    uint32_t graph_col[PM_GRAPH_W];
} PowerMeter;

typedef struct {
    PowerMeter* app;
} PmModel;

/* pm_meter.c */
void pm_exti_snapshot(PowerMeter* app);
bool pm_pin_available(const PowerMeter* app, uint8_t index);
void pm_capture_start(PowerMeter* app);
void pm_capture_stop(PowerMeter* app);
void pm_capture_restart(PowerMeter* app);
void pm_session_reset(PowerMeter* app);
void pm_tick(void* ctx);
uint32_t pm_instant_watts(const PowerMeter* app);
uint32_t pm_window_watts(const PowerMeter* app, uint32_t span_sec, bool* partial);

/* pm_settings.c */
void pm_config_set_defaults(PmConfig* cfg);
void pm_config_load(PowerMeter* app);
void pm_config_save(PowerMeter* app);
void pm_settings_build(PowerMeter* app);

/* pm_view.c */
void pm_view_draw(Canvas* canvas, void* model);
bool pm_view_input(InputEvent* event, void* ctx);
void pm_view_refresh(PowerMeter* app);
