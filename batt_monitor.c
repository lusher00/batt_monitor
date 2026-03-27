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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <robotcontrol.h>

/* ── Divider: R_bottom / (R_top + R_bottom) ────────────────────────── */
#define R_TOP        68000.0f
#define R_BOT        10000.0f
#define DIVIDER_RATIO  ((R_TOP + R_BOT) / R_BOT)   /* = 7.8 */

#define ADC_CHANNEL      1      /* AIN1 on the JST-SH ADC connector    */

#define VBAT_WARNING     10.5f  /* 3.50V/cell — log warning            */
#define VBAT_CRITICAL     9.6f  /* 3.20V/cell — shutdown               */

#define WATCH_INTERVAL_S  60
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

    /* stderr always (journald picks it up) */
    fprintf(stderr, "[batt_monitor] %s  %s  %.2fV\n", ts, level, vbat);

    /* also append to log file */
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        fprintf(f, "%s  %s  %.2fV\n", ts, level, vbat);
        fclose(f);
    }
}

static float read_vbat(void)
{
    float vadc = rc_adc_read_volt(ADC_CHANNEL);
    if (vadc < 0.0f) {
        fprintf(stderr, "[batt_monitor] ERROR: rc_adc_read_volt() failed\n");
        return -1.0f;
    }
    return vadc * DIVIDER_RATIO;
}

static void do_shutdown(float vbat)
{
    log_msg("CRITICAL — shutting down", vbat);
    /* give the log a moment to flush before power dies */
    sync();
    sleep(2);
    system("shutdown -h now 'Battery critical'");
}

/* ── modes ───────────────────────────────────────────────────────────── */

/* Single-shot check — called early in boot before heavy services start.
 * Returns 0 (OK), 1 (critical — caller should abort boot or shut down). */
static int mode_check(void)
{
    float vbat = read_vbat();
    if (vbat < 0.0f) return 0;   /* ADC error — don't block boot */

    if (vbat <= VBAT_CRITICAL) {
        write_status(vbat, "critical");
        log_msg("CRITICAL (boot check)", vbat);
        do_shutdown(vbat);
        return 1;
    }
    if (vbat <= VBAT_WARNING) {
        write_status(vbat, "warning");
        log_msg("WARNING  (boot check)", vbat);
    } else {
        write_status(vbat, "ok");
        log_msg("OK       (boot check)", vbat);
    }
    return 0;
}

/* Continuous watch — runs as a systemd service, checks every 60 s. */
static void mode_watch(void)
{
    float vbat0 = read_vbat();
    write_status(vbat0, vbat0 <= VBAT_CRITICAL ? "critical" : vbat0 <= VBAT_WARNING ? "warning" : "ok");
    log_msg("OK       (watch start)", vbat0);

    while (1) {
        sleep(WATCH_INTERVAL_S);

        float vbat = read_vbat();
        if (vbat < 0.0f) continue;  /* transient ADC error, keep going */

        if (vbat <= VBAT_CRITICAL) {
            write_status(vbat, "critical");
            do_shutdown(vbat);
            return;
        }
        if (vbat <= VBAT_WARNING) {
            write_status(vbat, "warning");
            log_msg("WARNING ", vbat);
        } else {
            write_status(vbat, "ok");
            log_msg("OK      ", vbat);
        }
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
