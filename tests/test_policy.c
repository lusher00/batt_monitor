#include <assert.h>
#include <stdio.h>
#include <time.h>

#define main batt_monitor_program_main
#include "../batt_monitor.c"
#undef main

static void reset_policy(void)
{
    cfg.shutdown_enabled = 1;
    cfg.v_warning = 10.5f;
    cfg.v_low = 9.9f;
    cfg.v_critical = 9.6f;
    cfg.confirm_samples = 3;
    cfg.trend_seconds = 7200;
    cfg.trend_drop = 0.30f;
    critical_samples = 0;
    warning_since = 0;
    warning_start_voltage = 0.0f;
    shutdown_requested = 0;
    shutdown_event = 0;
}

int main(void)
{
    reset_policy();
    assert(!shutdown_policy(9.55f));
    assert(!shutdown_policy(9.54f));
    assert(shutdown_policy(9.53f));

    reset_policy();
    warning_since = time(NULL) - 7201;
    warning_start_voltage = 10.2f;
    assert(shutdown_policy(9.55f));

    reset_policy();
    cfg.shutdown_enabled = 0;
    assert(!shutdown_policy(9.4f));
    assert(!shutdown_policy(9.3f));
    assert(!shutdown_policy(9.2f));

    puts("battery shutdown policy tests passed");
    return 0;
}
