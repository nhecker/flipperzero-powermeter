#include "powermeter.h"

static bool pm_custom_event(void* ctx, uint32_t event) {
    PowerMeter* app = ctx;
    switch(event) {
    case PmEventSettings:
        view_dispatcher_switch_to_view(app->view_dispatcher, PmViewSettings);
        return true;
    case PmEventExit:
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    default:
        return false;
    }
}

static uint32_t pm_settings_previous(void* ctx) {
    UNUSED(ctx);
    return PmViewMain;
}

static PowerMeter* pm_alloc(void) {
    PowerMeter* app = malloc(sizeof(PowerMeter));
    memset(app, 0, sizeof(PowerMeter));

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->storage = furi_record_open(RECORD_STORAGE);

    /* Before anything of ours touches EXTI, so the snapshot sees only the
     * lines the firmware itself owns. */
    pm_exti_snapshot(app);

    pm_config_load(app);
    pm_ring_reset(&app->ring);
    app->start_tick = furi_get_tick();
    app->rng = furi_get_tick() | 1u;
    app->page = PmPageLive;

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, pm_custom_event);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, pm_tick, PM_TICK_MS);

    app->main_view = view_alloc();
    view_allocate_model(app->main_view, ViewModelTypeLocking, sizeof(PmModel));
    with_view_model(app->main_view, PmModel * model, { model->app = app; }, false);
    view_set_context(app->main_view, app);
    view_set_draw_callback(app->main_view, pm_view_draw);
    view_set_input_callback(app->main_view, pm_view_input);

    app->settings_list = variable_item_list_alloc();
    pm_settings_build(app);
    view_set_previous_callback(
        variable_item_list_get_view(app->settings_list), pm_settings_previous);

    view_dispatcher_add_view(app->view_dispatcher, PmViewMain, app->main_view);
    view_dispatcher_add_view(
        app->view_dispatcher, PmViewSettings, variable_item_list_get_view(app->settings_list));

    return app;
}

static void pm_free(PowerMeter* app) {
    pm_capture_stop(app);
    pm_config_save(app);

    view_dispatcher_remove_view(app->view_dispatcher, PmViewMain);
    view_dispatcher_remove_view(app->view_dispatcher, PmViewSettings);
    variable_item_list_free(app->settings_list);
    view_free(app->main_view);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t powermeter_app(void* p) {
    UNUSED(p);
    PowerMeter* app = pm_alloc();

    pm_capture_start(app);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, PmViewMain);
    view_dispatcher_run(app->view_dispatcher);

    pm_free(app);
    return 0;
}
