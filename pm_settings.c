#include "powermeter.h"

#include <flipper_format/flipper_format.h>
#include <stdio.h>

static const uint32_t pm_imp_values[] =
    {100, 200, 320, 400, 500, 800, 1000, 1600, 2000, 3200, 10000};
static const uint32_t pm_pull_values[] = {GpioPullUp, GpioPullDown, GpioPullNo};
static const char* const pm_pull_names[] = {"Up", "Down", "None"};
static const uint32_t pm_min_values[] = {1, 2, 3, 5, 10, 20, 30, 50};
static const uint32_t pm_max_values[] = {50, 100, 150, 200, 250, 300, 500, 1000};
static const uint32_t pm_demo_values[] = {100, 250, 500, 1000, 2000, 3500, 5000, 8000, 12000};
static const char* const pm_source_names[] = {"GPIO", "IR", "Demo"};
static const char* const pm_level_names[] = {"Low", "High"};

/* VariableItemList copies the value text, but each item keeps its own buffer so
 * the code stays correct regardless. */
static char pm_buf_pin[16];
static char pm_buf_imp[12];
static char pm_buf_min[12];
static char pm_buf_max[12];
static char pm_buf_demo[12];

static uint8_t pm_index_of(const uint32_t* values, size_t count, uint32_t needle) {
    for(size_t i = 0; i < count; i++) {
        if(values[i] == needle) return (uint8_t)i;
    }
    return 0;
}

void pm_config_set_defaults(PmConfig* cfg) {
    cfg->imp_per_kwh = 1000;
    cfg->pin_index = 0;
    cfg->pull = GpioPullUp;
    cfg->active_high = false;
    cfg->min_pulse_ms = 3;
    cfg->max_pulse_ms = 250;
    cfg->source = PmSourceGpio;
    cfg->demo_watts = 1000;
}

static uint32_t pm_read_u32(FlipperFormat* ff, const char* key, uint32_t fallback) {
    uint32_t v = 0;
    flipper_format_rewind(ff);
    if(!flipper_format_read_uint32(ff, key, &v, 1)) return fallback;
    return v;
}

static bool pm_read_bool(FlipperFormat* ff, const char* key, bool fallback) {
    bool v = false;
    flipper_format_rewind(ff);
    if(!flipper_format_read_bool(ff, key, &v, 1)) return fallback;
    return v;
}

void pm_config_load(PowerMeter* app) {
    PmConfig* cfg = &app->cfg;
    pm_config_set_defaults(cfg);

    FlipperFormat* ff = flipper_format_file_alloc(app->storage);
    FuriString* type = furi_string_alloc();
    uint32_t version = 0;

    do {
        if(!flipper_format_file_open_existing(ff, PM_CONFIG_PATH)) break;
        if(!flipper_format_read_header(ff, type, &version)) break;
        if(furi_string_cmp_str(type, PM_CONFIG_HEADER) != 0) break;

        cfg->imp_per_kwh = pm_read_u32(ff, "imp_per_kwh", cfg->imp_per_kwh);
        cfg->pin_index = (uint8_t)pm_read_u32(ff, "pin_index", cfg->pin_index);
        cfg->pull = (uint8_t)pm_read_u32(ff, "pull", cfg->pull);
        cfg->min_pulse_ms = (uint16_t)pm_read_u32(ff, "min_pulse_ms", cfg->min_pulse_ms);
        cfg->max_pulse_ms = (uint16_t)pm_read_u32(ff, "max_pulse_ms", cfg->max_pulse_ms);
        cfg->source = (uint8_t)pm_read_u32(ff, "source", cfg->source);
        cfg->demo_watts = pm_read_u32(ff, "demo_watts", cfg->demo_watts);
        cfg->active_high = pm_read_bool(ff, "active_high", cfg->active_high);
    } while(false);

    furi_string_free(type);
    flipper_format_free(ff);

    if(cfg->imp_per_kwh == 0) cfg->imp_per_kwh = 1000;
    if(cfg->pin_index >= pm_pin_count) cfg->pin_index = 0;
    /* A config naming a pin the firmware now owns must not wedge the app on
     * every launch, so fall forward to the first line that is actually free. */
    if(!pm_pin_available(app, cfg->pin_index)) {
        for(uint8_t i = 0; i < pm_pin_count; i++) {
            if(pm_pin_available(app, i)) {
                cfg->pin_index = i;
                break;
            }
        }
    }
    if(cfg->source >= PmSourceCount) cfg->source = PmSourceGpio;
    if(cfg->pull > GpioPullDown) cfg->pull = GpioPullUp;
    if(cfg->max_pulse_ms <= cfg->min_pulse_ms) {
        cfg->min_pulse_ms = 3;
        cfg->max_pulse_ms = 250;
    }
}

void pm_config_save(PowerMeter* app) {
    PmConfig* cfg = &app->cfg;
    storage_simply_mkdir(app->storage, STORAGE_APP_DATA_PATH_PREFIX);

    FlipperFormat* ff = flipper_format_file_alloc(app->storage);
    do {
        if(!flipper_format_file_open_always(ff, PM_CONFIG_PATH)) break;
        if(!flipper_format_write_header_cstr(ff, PM_CONFIG_HEADER, PM_CONFIG_VERSION)) break;

        uint32_t v;
        v = cfg->imp_per_kwh;
        flipper_format_write_uint32(ff, "imp_per_kwh", &v, 1);
        v = cfg->pin_index;
        flipper_format_write_uint32(ff, "pin_index", &v, 1);
        v = cfg->pull;
        flipper_format_write_uint32(ff, "pull", &v, 1);
        v = cfg->min_pulse_ms;
        flipper_format_write_uint32(ff, "min_pulse_ms", &v, 1);
        v = cfg->max_pulse_ms;
        flipper_format_write_uint32(ff, "max_pulse_ms", &v, 1);
        v = cfg->source;
        flipper_format_write_uint32(ff, "source", &v, 1);
        v = cfg->demo_watts;
        flipper_format_write_uint32(ff, "demo_watts", &v, 1);

        bool b;
        b = cfg->active_high;
        flipper_format_write_bool(ff, "active_high", &b, 1);
    } while(false);

    flipper_format_free(ff);
}

static void pm_on_source(VariableItem* item) {
    PowerMeter* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, pm_source_names[i]);
    app->cfg.source = i;
    pm_capture_stop(app);
    pm_capture_start(app);
}

static void pm_on_imp(VariableItem* item) {
    PowerMeter* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    app->cfg.imp_per_kwh = pm_imp_values[i];
    snprintf(pm_buf_imp, sizeof(pm_buf_imp), "%lu", (unsigned long)pm_imp_values[i]);
    variable_item_set_current_value_text(item, pm_buf_imp);
}

/* "PA4 line4" when usable, "PA4 busy" when the firmware owns that line. */
static void pm_pin_text(PowerMeter* app, uint8_t i, char* out, size_t len) {
    if(pm_pin_available(app, i)) {
        snprintf(out, len, "%s line%u", pm_pins[i].label, (unsigned)pm_pin_line(i));
    } else {
        snprintf(out, len, "%s busy", pm_pins[i].label);
    }
}

static void pm_on_pin(VariableItem* item) {
    PowerMeter* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    app->cfg.pin_index = i;
    pm_pin_text(app, i, pm_buf_pin, sizeof(pm_buf_pin));
    variable_item_set_current_value_text(item, pm_buf_pin);
    pm_capture_restart(app);
}

static void pm_on_pull(VariableItem* item) {
    PowerMeter* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, pm_pull_names[i]);
    app->cfg.pull = (uint8_t)pm_pull_values[i];
    pm_capture_restart(app);
}

static void pm_on_level(VariableItem* item) {
    PowerMeter* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, pm_level_names[i]);
    app->cfg.active_high = (i == 1);
}

static void pm_on_min(VariableItem* item) {
    PowerMeter* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    app->cfg.min_pulse_ms = (uint16_t)pm_min_values[i];
    snprintf(pm_buf_min, sizeof(pm_buf_min), "%lums", (unsigned long)pm_min_values[i]);
    variable_item_set_current_value_text(item, pm_buf_min);
}

static void pm_on_max(VariableItem* item) {
    PowerMeter* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    app->cfg.max_pulse_ms = (uint16_t)pm_max_values[i];
    snprintf(pm_buf_max, sizeof(pm_buf_max), "%lums", (unsigned long)pm_max_values[i]);
    variable_item_set_current_value_text(item, pm_buf_max);
}

static void pm_on_demo(VariableItem* item) {
    PowerMeter* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    app->cfg.demo_watts = pm_demo_values[i];
    snprintf(pm_buf_demo, sizeof(pm_buf_demo), "%luW", (unsigned long)pm_demo_values[i]);
    variable_item_set_current_value_text(item, pm_buf_demo);
}

#define PM_ITEM_RESET 7

static void pm_on_enter(void* context, uint32_t index) {
    PowerMeter* app = context;
    if(index != PM_ITEM_RESET) return;
    pm_session_reset(app);
    notification_message(app->notifications, &sequence_success);
}

void pm_settings_build(PowerMeter* app) {
    VariableItemList* list = app->settings_list;
    VariableItem* item;
    uint8_t idx;

    item = variable_item_list_add(list, "Source", PmSourceCount, pm_on_source, app);
    variable_item_set_current_value_index(item, app->cfg.source);
    variable_item_set_current_value_text(item, pm_source_names[app->cfg.source]);

    item = variable_item_list_add(list, "Pulses/kWh", COUNT_OF(pm_imp_values), pm_on_imp, app);
    idx = pm_index_of(pm_imp_values, COUNT_OF(pm_imp_values), app->cfg.imp_per_kwh);
    variable_item_set_current_value_index(item, idx);
    snprintf(pm_buf_imp, sizeof(pm_buf_imp), "%lu", (unsigned long)pm_imp_values[idx]);
    variable_item_set_current_value_text(item, pm_buf_imp);
    app->cfg.imp_per_kwh = pm_imp_values[idx];

    item = variable_item_list_add(list, "GPIO pin", (uint8_t)pm_pin_count, pm_on_pin, app);
    variable_item_set_current_value_index(item, app->cfg.pin_index);
    pm_pin_text(app, app->cfg.pin_index, pm_buf_pin, sizeof(pm_buf_pin));
    variable_item_set_current_value_text(item, pm_buf_pin);

    item =
        variable_item_list_add(list, "Internal pull", COUNT_OF(pm_pull_values), pm_on_pull, app);
    idx = pm_index_of(pm_pull_values, COUNT_OF(pm_pull_values), app->cfg.pull);
    variable_item_set_current_value_index(item, idx);
    variable_item_set_current_value_text(item, pm_pull_names[idx]);

    item = variable_item_list_add(list, "Pulse level", 2, pm_on_level, app);
    variable_item_set_current_value_index(item, app->cfg.active_high ? 1 : 0);
    variable_item_set_current_value_text(item, pm_level_names[app->cfg.active_high ? 1 : 0]);

    item = variable_item_list_add(list, "Min pulse", COUNT_OF(pm_min_values), pm_on_min, app);
    idx = pm_index_of(pm_min_values, COUNT_OF(pm_min_values), app->cfg.min_pulse_ms);
    variable_item_set_current_value_index(item, idx);
    snprintf(pm_buf_min, sizeof(pm_buf_min), "%lums", (unsigned long)pm_min_values[idx]);
    variable_item_set_current_value_text(item, pm_buf_min);
    app->cfg.min_pulse_ms = (uint16_t)pm_min_values[idx];

    item = variable_item_list_add(list, "Max pulse", COUNT_OF(pm_max_values), pm_on_max, app);
    idx = pm_index_of(pm_max_values, COUNT_OF(pm_max_values), app->cfg.max_pulse_ms);
    variable_item_set_current_value_index(item, idx);
    snprintf(pm_buf_max, sizeof(pm_buf_max), "%lums", (unsigned long)pm_max_values[idx]);
    variable_item_set_current_value_text(item, pm_buf_max);
    app->cfg.max_pulse_ms = (uint16_t)pm_max_values[idx];

    item = variable_item_list_add(list, "Reset stats", 1, NULL, app);
    variable_item_set_current_value_text(item, "OK");

    item = variable_item_list_add(list, "Demo load", COUNT_OF(pm_demo_values), pm_on_demo, app);
    idx = pm_index_of(pm_demo_values, COUNT_OF(pm_demo_values), app->cfg.demo_watts);
    variable_item_set_current_value_index(item, idx);
    snprintf(pm_buf_demo, sizeof(pm_buf_demo), "%luW", (unsigned long)pm_demo_values[idx]);
    variable_item_set_current_value_text(item, pm_buf_demo);
    app->cfg.demo_watts = pm_demo_values[idx];

    variable_item_list_set_enter_callback(list, pm_on_enter, app);
}
