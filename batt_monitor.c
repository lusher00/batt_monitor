/**
 * batt_monitor.c
 *
 * Reads 3S LiPo voltage via a resistor divider on AIN0.
 * Divider: 10k (top) + 68k (bottom) → Vbat/7.8 on AIN0.
 * ADC ref = 1.8V, 12-bit (0–4095).
 *
 * Modes:
 *   --check   single shot, exits 0 if OK, 1 if critical (for boot oneshot)
 *   --watch   loop every 60s, logs + shuts down at critical (for service)
 *
 * Thresholds (3S LiPo):
 *   WARNING  : 10.5V  (3.50V/cell)
 *   CRITICAL : 9.6V   (3.20V/cell) → graceful shutdown
 *
 * Averaging:
 *   SAMPLE_COUNT samples are taken with SAMPLE_DELAY_US between them.
 *   The min and max are discarded, the rest are averaged. This rejects
 *   ADC glitches and gives a stable reading.
 *
 * Grace window:
 *   For GRACE_PERIOD_S seconds after first boot, critical voltage is
 *   logged but does NOT trigger shutdown. This lets you SSH in and
 *   disable the service when running off a 5V bench supply.
 *   Grace state is tracked via GRACE_STAMP_FILE (cleared on reboot
 *   because it lives in /run).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <robotcontrol.h>

/* ── Divider: R_bottom / (R_top + R_bottom) ────────────────────────── */
#define R_TOP           68000.0f
#define R_BOT           10000.0f
#define DIVIDER_RATIO   ((R_TOP + R_BOT) / R_BOT)   /* = 7.8 */

#define ADC_CHANNEL     1       /* AIN1 on the JST-SH ADC connector    */

#define VBAT_WARNING    10.5f   /* 3.50V/cell — log warning            */
#define VBAT_CRITICAL    9.6f   /* 3.20V/cell — shutdown               */

/* ── Averaging ───────────────────────────────────────────────────────── */
#define SAMPLE_COUNT    32      /* total samples per read               */
#define SAMPLE_DELAY_US 2000    /* 2 ms between samples → ~64 ms total  */
/* min and max are always discarded; need at least 3 samples */
#if SAMPLE_COUNT < 3
#error SAMPLE_COUNT must be at least 3
#endif

/* ── Grace window ────────────────────────────────────────────────────── */
#define GRACE_PERIOD_S  180     /* 3 minutes after boot before shutdown  */
#define GRACE_STAMP_FILE "/run/batt_grace_start"   /* cleared on reboot */

/* ── Misc ────────────────────────────────────────────────────────────── */
#define WATCH_INTERVAL_S 60
#define LOG_FILE         "/var/log/batt_monitor.log"


/* ── helpers ─────────────────────────────────────────────────────────── */

static void write_status(float vbat, const char *status)
{
    FILE *f = fopen("/run/batt_status.json", "w");
    if (!f) return;
    fprintf(f, "{\"voltage\":%.3f,\"status\":\"%s\"}\n", vbat, status);
    fclose(f);
}

static void log_msg(const char *level, float vbat)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);

    fprintf(stderr, "[batt_monitor] %s  %s  %.2fV\n", ts, level, vbat);

    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        fprintf(f, "%s  %s  %.2fV\n", ts, level, vbat);
        fclose(f);
    }
}

/* Write a timestamp file the first time we run this boot. */
static void grace_stamp_init(void)
{
    FILE *f = fopen(GRACE_STAMP_FILE, "wx");   /* x = fail if exists */
    if (!f) return;   /* already stamped this boot */
    fprintf(f, "%ld\n", (long)time(NULL));
    fclose(f);
}

/* Returns 1 if we are still inside the grace window, 0 if expired. */
static int grace_active(void)
{
    FILE *f = fopen(GRACE_STAMP_FILE, "r");
    if (!f) return 0;   /* no stamp — should not happen, but don't block */

    long stamp = 0;
    fscanf(f, "%ld", &stamp);
    fclose(f);

    long elapsed = (long)time(NULL) - stamp;
    return (elapsed < GRACE_PERIOD_S);
}

/* Compare function for qsort float array. */
static int cmp_float(const void *a, const void *b)
{
    float fa = *(const float *)a;
    float fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

/* Take SAMPLE_COUNT ADC reads, discard min+max, return average.
 * Returns -1.0f on ADC error. */
static float read_vbat(void)
{
    float samples[SAMPLE_COUNT];
    int valid = 0;

    for (int i = 0; i < SAMPLE_COUNT; i++) {
        float v = rc_adc_read_volt(ADC_CHANNEL);
        if (v < 0.0f) {
            fprintf(stderr, "[batt_monitor] WARNING: ADC read %d failed, skipping\n", i);
            continue;
        }
        samples[valid++] = v;
        if (i < SAMPLE_COUNT - 1)
            usleep(SAMPLE_DELAY_US);
    }

    if (valid < 3) {
        fprintf(stderr, "[batt_monitor] ERROR: only %d valid ADC samples\n", valid);
        return -1.0f;
    }

    qsort(samples, valid, sizeof(float), cmp_float);

    /* Discard lowest and highest sample. */
    float sum = 0.0f;
    for (int i = 1; i < valid - 1; i++)
        sum += samples[i];

    float vadc_avg = sum / (float)(valid - 2);
    return vadc_avg * DIVIDER_RATIO;
}

static void do_shutdown(float vbat)
{
    log_msg("CRITICAL — shutting down", vbat);
    sync();
    sleep(2);
    system("shutdown -h now 'Battery critical'");
}

/* Shared logic: decide what to do given a fresh vbat reading.
 * Returns 1 if shutdown was triggered, 0 otherwise. */
static int evaluate_voltage(float vbat, const char *context)
{
    if (vbat < 0.0f) return 0;   /* ADC error — don't block */

    if (vbat <= VBAT_CRITICAL) {
        if (grace_active()) {
            /* Still in grace window — warn loudly but don't shut down. */
            char msg[64];
            snprintf(msg, sizeof(msg), "CRITICAL (grace window) [%s]", context);
            write_status(vbat, "critical-grace");
            log_msg(msg, vbat);
            fprintf(stderr, "[batt_monitor] Shutdown suppressed — grace window active. "
                            "Disable batt_check.service if on 5V supply.\n");
            return 0;
        }
        write_status(vbat, "critical");
        char msg[64];
        snprintf(msg, sizeof(msg), "CRITICAL [%s]", context);
        log_msg(msg, vbat);
        do_shutdown(vbat);
        return 1;
    }

    if (vbat <= VBAT_WARNING) {
        write_status(vbat, "warning");
        char msg[64];
        snprintf(msg, sizeof(msg), "WARNING  [%s]", context);
        log_msg(msg, vbat);
    } else {
        write_status(vbat, "ok");
        char msg[64];
        snprintf(msg, sizeof(msg), "OK       [%s]", context);
        log_msg(msg, vbat);
    }
    return 0;
}

/* ── modes ───────────────────────────────────────────────────────────── */

static int mode_check(void)
{
    grace_stamp_init();   /* record boot time on first run */

    float vbat = read_vbat();
    return evaluate_voltage(vbat, "boot check");
}

static void mode_watch(void)
{
    /* Grace stamp should already exist from --check, but stamp anyway
     * in case watch runs standalone. */
    grace_stamp_init();

    float vbat0 = read_vbat();
    evaluate_voltage(vbat0, "watch start");

    while (1) {
        sleep(WATCH_INTERVAL_S);

        float vbat = read_vbat();
        if (vbat < 0.0f) continue;   /* transient ADC error, keep going */

        if (evaluate_voltage(vbat, "watch")) return;   /* shutdown triggered */
    }
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s --check | --watch\n", argv[0]);
        return 2;
    }

    if (rc_adc_init() < 0) {
        fprintf(stderr, "[batt_monitor] rc_adc_init() failed\n");
        return 0;   /* don't block boot if ADC init fails */
    }

    int ret = 0;
    if (strcmp(argv[1], "--check") == 0) {
        ret = mode_check();
    } else if (strcmp(argv[1], "--watch") == 0) {
        mode_watch();
    } else {
        fprintf(stderr, "Unknown mode: %s\n", argv[1]);
        ret = 2;
    }

    rc_adc_cleanup();
    return ret;
}