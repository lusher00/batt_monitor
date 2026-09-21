// Copyright (c) 2025 Ryan Lush <ryan.lush@gmail.com>
//
// Free for personal, educational, and open-source use.
// Commercial use requires written permission from the author.
// Contact: ryan.lush@gmail.com
// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Ryan Lush <ryan.lush@gmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
/**
 * batt_monitor.c
 *
 * Reads pack voltage on the BeagleBone Blue's built-in DC power jack sense,
 * AIN5 (net B8 on the schematic).
 * Divider: 47k (top) + 4.7k (bottom) → Vjack/11.0 on AIN5.
 * ADC ref = 1.8V, 12-bit (0–4095).
 *
 * Modes:
 *   --check     single shot, logs voltage + status, exits 0 always
 *   --watch     loop (default 60s), logs warnings
 *   --print     print raw voltage to stdout as "12.34" and exit (scripting)
 *   --calibrate V   read the ADC against a known input voltage V and print
 *                   the divider ratio that makes the reading match
 *
 * Threshold flags (volts):
 *   --warning V     default 10.5  (3.50V/cell × 3S)
 *   --low V         default  9.9  (3.30V/cell × 3S)
 *   --critical V    default  9.6  (3.20V/cell × 3S)
 *
 * Hardware flags:
 *   --cells N       set cell count (2–6); scales all default thresholds
 *   --divider R     override resistor divider ratio (default 11.0)
 *   --channel N     override ADC channel (default 5)
 *
 * Timing flags:
 *   --interval S    watch loop interval in seconds (default 10)
 *
 * Safety flags:
 *   --shutdown      enable graceful shutdown at critical threshold (default: OFF)
 *
 * Examples:
 *   batt_monitor --check
 *   batt_monitor --watch --shutdown
 *   batt_monitor --watch --interval 30 --warning 10.8 --critical 9.9
 *   batt_monitor --cells 4 --watch --shutdown
 *   batt_monitor --print
 *   batt_monitor --channel 5 --calibrate 12.00
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
/* ADC via sysfs IIO — no librobotcontrol needed */
/* BBB Blue: /sys/bus/iio/devices/iio:device0/in_voltageN_raw, ref=1.8V, 12-bit */
#define ADC_IIO_PATH   "/sys/bus/iio/devices/iio:device0"
#define ADC_REF_V      1.8f
#define ADC_MAX_RAW    4096.0f

static float rc_adc_read_volt(int channel)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/in_voltage%d_raw", ADC_IIO_PATH, channel);
    FILE *f = fopen(path, "r");
    if (!f) return -1.0f;
    int raw = 0;
    if (fscanf(f, "%d", &raw) != 1) {
        fclose(f);
        return -1.0f;
    }
    fclose(f);
    return (raw / ADC_MAX_RAW) * ADC_REF_V;
}
static int  rc_adc_init(void)    { return 0; }
static void rc_adc_cleanup(void) {}

/* ── fallback defaults ───────────────────────────────────────────────────
 * These exist only so the program runs with no flags. The installed systemd
 * units pass every hardware and threshold value explicitly from
 * /etc/default/batt_monitor, so changing hardware never needs a rebuild.
 */
#define DEFAULT_R_TOP          47000.0f
#define DEFAULT_R_BOT           4700.0f
#define DEFAULT_DIVIDER_RATIO  ((DEFAULT_R_TOP + DEFAULT_R_BOT) / DEFAULT_R_BOT)  /* 11.0 */
#define DEFAULT_ADC_CHANNEL    5
#define DEFAULT_WATCH_INTERVAL 10
#define DEFAULT_CONFIRM_SAMPLES 3
#define DEFAULT_TREND_SECONDS  7200
#define DEFAULT_TREND_DROP     0.30f
#define DEFAULT_SHUTDOWN_GRACE 15

/* Per-cell thresholds (volts) — scaled by cell count */
#define VCELL_WARNING   3.50f
#define VCELL_LOW       3.30f
#define VCELL_CRITICAL  3.20f
#define DEFAULT_CELLS   3

#define LOG_FILE  "/var/log/batt_monitor.log"

/* ── config struct ───────────────────────────────────────────────────── */
typedef struct {
    float divider;
    int   channel;
    int   interval_s;
    float v_warning;
    float v_low;
    float v_critical;
    int   shutdown_enabled;
    int   confirm_samples;
    int   trend_seconds;
    float trend_drop;
    int   shutdown_grace_s;
} Config;

static Config cfg;
static int critical_samples;
static time_t warning_since;
static float warning_start_voltage;
static int shutdown_requested;
static long long shutdown_event;

/* ── helpers ─────────────────────────────────────────────────────────── */

static void write_status(float vbat, const char *status)
{
    const char *tmp = "/run/batt_status.json.tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f,
            "{\"voltage\":%.3f,\"status\":\"%s\","
            "\"shutdown_enabled\":%d,\"critical_samples\":%d,"
            "\"shutdown_requested\":%d,\"shutdown_event\":%lld,"
            "\"updated\":%lld}\n",
            vbat, status, cfg.shutdown_enabled, critical_samples,
            shutdown_requested, shutdown_event, (long long)time(NULL));
    if (fclose(f) == 0)
        rename(tmp, "/run/batt_status.json");
    else
        unlink(tmp);
}

static void log_msg(const char *level, float vbat)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);

    fprintf(stderr, "[batt_monitor] %s  %-30s  %.2fV\n", ts, level, vbat);

    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        fprintf(f, "%s  %-30s  %.2fV\n", ts, level, vbat);
        fclose(f);
    }
}

static float read_vbat(void)
{
    float vadc = rc_adc_read_volt(cfg.channel);
    if (vadc < 0.0f) {
        fprintf(stderr, "[batt_monitor] ERROR: rc_adc_read_volt(%d) failed\n", cfg.channel);
        return -1.0f;
    }
    return vadc * cfg.divider;
}

static const char *classify(float vbat, const char **log_label)
{
    if (vbat <= cfg.v_critical) {
        if (log_label) *log_label = "CRITICAL";
        return "critical";
    }
    if (vbat <= cfg.v_low) {
        if (log_label) *log_label = "LOW     ";
        return "low";
    }
    if (vbat <= cfg.v_warning) {
        if (log_label) *log_label = "WARNING ";
        return "warning";
    }
    if (log_label) *log_label = "OK      ";
    return "ok";
}

static int shutdown_policy(float vbat)
{
    time_t now = time(NULL);

    if (vbat <= cfg.v_warning) {
        if (!warning_since) {
            warning_since = now;
            warning_start_voltage = vbat;
        }
    } else {
        warning_since = 0;
        warning_start_voltage = 0.0f;
    }

    if (vbat <= cfg.v_critical)
        critical_samples++;
    else
        critical_samples = 0;

    int trend_qualified = warning_since
        && now - warning_since >= cfg.trend_seconds
        && warning_start_voltage - vbat >= cfg.trend_drop;

    return cfg.shutdown_enabled
        && vbat <= cfg.v_critical
        && (critical_samples >= cfg.confirm_samples || trend_qualified);
}

static void do_shutdown(float vbat)
{
    shutdown_requested = 1;
    shutdown_event = (long long)time(NULL);
    write_status(vbat, "critical");
    log_msg("CRITICAL — shutdown requested", vbat);
    sync();
    sleep(cfg.shutdown_grace_s);
    int rc = system("shutdown -h now 'Battery critical'");
    if (rc != 0)
        fprintf(stderr, "[batt_monitor] shutdown command failed: %d\n", rc);
}

/* ── modes ───────────────────────────────────────────────────────────── */

static int mode_check(void)
{
    int attempts = cfg.shutdown_enabled ? cfg.confirm_samples : 1;
    for (int i = 0; i < attempts; i++) {
        float vbat = read_vbat();
        if (vbat < 0.0f) {
            fprintf(stderr, "[batt_monitor] ADC error — skipping\n");
            return 0;
        }

        const char *log_label;
        const char *status = classify(vbat, &log_label);
        int trigger = shutdown_policy(vbat);
        write_status(vbat, status);
        log_msg(log_label, vbat);

        if (trigger) {
            do_shutdown(vbat);
            break;
        }
        if (vbat > cfg.v_critical)
            break;
        if (i + 1 < attempts)
            sleep(2);
    }

    return 0;  /* always 0 — don't let systemd abort boot */
}

static void mode_watch(void)
{
    while (1) {
        float vbat = read_vbat();
        if (vbat < 0.0f) {
            sleep(cfg.interval_s);
            continue;
        }

        const char *log_label;
        const char *status = classify(vbat, &log_label);
        int trigger = shutdown_policy(vbat);
        write_status(vbat, status);
        log_msg(log_label, vbat);

        if (trigger) {
            do_shutdown(vbat);
            return;
        }
        sleep(cfg.interval_s);
    }
}

/* Print a single voltage reading to stdout, no decoration — for scripts */
static void mode_print(void)
{
    float vbat = read_vbat();
    if (vbat < 0.0f)
        fprintf(stdout, "error\n");
    else
        fprintf(stdout, "%.3f\n", vbat);
}

/* Read a known input voltage and report the divider ratio that matches it.
 * Use with a bench supply or meter reading on the same node:
 *   batt_monitor --channel 5 --calibrate 12.00
 */
static void mode_calibrate(float v_ref)
{
    float vadc = rc_adc_read_volt(cfg.channel);
    if (vadc < 0.0f) {
        fprintf(stderr, "[batt_monitor] ERROR: cannot read ADC channel %d\n", cfg.channel);
        return;
    }
    if (vadc < 0.001f) {
        fprintf(stderr, "[batt_monitor] ERROR: ADC channel %d reads ~0V — "
                        "wrong channel, or nothing connected\n", cfg.channel);
        return;
    }

    float ratio = v_ref / vadc;

    printf("ADC channel      : %d\n", cfg.channel);
    printf("ADC pin voltage  : %.4f V\n", vadc);
    printf("Reference input  : %.3f V\n", v_ref);
    printf("Current divider  : %.4f  -> reports %.3f V  (error %+.3f V)\n",
           cfg.divider, vadc * cfg.divider, vadc * cfg.divider - v_ref);
    printf("Corrected divider: %.4f\n", ratio);
    printf("\nPut this in /etc/default/batt_monitor:  --divider %.4f\n", ratio);
}

/* ── arg parsing ─────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s MODE [OPTIONS]\n"
        "\n"
        "Modes:\n"
        "  --check          Single shot voltage check, log result\n"
        "  --watch          Continuous watch loop\n"
        "  --print          Print voltage to stdout and exit\n"
        "  --calibrate V    Read ADC against known input V, print correct divider\n"
        "\n"
        "Threshold options (volts):\n"
        "  --warning  V     Warning threshold  (default %.2fV)\n"
        "  --low      V     Low threshold      (default %.2fV)\n"
        "  --critical V     Critical threshold (default %.2fV)\n"
        "  --cells    N     Cell count: scales all default thresholds (default %d)\n"
        "\n"
        "Hardware options:\n"
        "  --divider  R     ADC voltage divider ratio (default %.1f)\n"
        "  --channel  N     ADC channel number (default %d)\n"
        "\n"
        "Timing options:\n"
        "  --interval S     Watch loop interval in seconds (default %d)\n"
        "  --confirm-samples N  Critical samples required (default %d)\n"
        "  --trend-hours H  Warning/low trend duration for fast critical shutdown (default 2)\n"
        "  --trend-drop V   Minimum drop across trend window (default %.2fV)\n"
        "  --shutdown-grace S  Seconds for peers to halt before Bone shutdown (default %d)\n"
        "\n"
        "Safety options:\n"
        "  --shutdown       Enable graceful shutdown at critical voltage (default: OFF)\n"
        "\n"
        "Examples:\n"
        "  %s --check\n"
        "  %s --watch --shutdown\n"
        "  %s --watch --interval 30 --warning 10.8 --critical 9.9\n"
        "  %s --cells 4 --watch --shutdown\n"
        "  %s --print\n"
        "  %s --channel 5 --calibrate 12.00\n",
        prog,
        VCELL_WARNING  * DEFAULT_CELLS,
        VCELL_LOW      * DEFAULT_CELLS,
        VCELL_CRITICAL * DEFAULT_CELLS,
        DEFAULT_CELLS,
        DEFAULT_DIVIDER_RATIO,
        DEFAULT_ADC_CHANNEL,
        DEFAULT_WATCH_INTERVAL,
        DEFAULT_CONFIRM_SAMPLES,
        DEFAULT_TREND_DROP,
        DEFAULT_SHUTDOWN_GRACE,
        prog, prog, prog, prog, prog, prog);
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    /* defaults */
    int cells = DEFAULT_CELLS;
    cfg.divider          = DEFAULT_DIVIDER_RATIO;
    cfg.channel          = DEFAULT_ADC_CHANNEL;
    cfg.interval_s       = DEFAULT_WATCH_INTERVAL;
    cfg.v_warning        = VCELL_WARNING  * cells;
    cfg.v_low            = VCELL_LOW      * cells;
    cfg.v_critical       = VCELL_CRITICAL * cells;
    cfg.shutdown_enabled = 0;
    cfg.confirm_samples  = DEFAULT_CONFIRM_SAMPLES;
    cfg.trend_seconds    = DEFAULT_TREND_SECONDS;
    cfg.trend_drop       = DEFAULT_TREND_DROP;
    cfg.shutdown_grace_s = DEFAULT_SHUTDOWN_GRACE;

    /* track explicit overrides so --cells doesn't clobber them */
    int warning_set  = 0;
    int low_set      = 0;
    int critical_set = 0;

    enum { MODE_NONE, MODE_CHECK, MODE_WATCH, MODE_PRINT, MODE_CAL } mode = MODE_NONE;
    float cal_ref = 0.0f;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) {
            mode = MODE_CHECK;
        } else if (strcmp(argv[i], "--watch") == 0) {
            mode = MODE_WATCH;
        } else if (strcmp(argv[i], "--print") == 0) {
            mode = MODE_PRINT;
        } else if (strcmp(argv[i], "--calibrate") == 0 && i + 1 < argc) {
            mode = MODE_CAL;
            cal_ref = atof(argv[++i]);
            if (cal_ref <= 0.0f) {
                fprintf(stderr, "Error: --calibrate needs a positive voltage\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--shutdown") == 0) {
            cfg.shutdown_enabled = 1;
        } else if (strcmp(argv[i], "--cells") == 0 && i + 1 < argc) {
            cells = atoi(argv[++i]);
            if (cells < 1 || cells > 8) {
                fprintf(stderr, "Error: --cells must be 1–8\n");
                return 2;
            }
            /* recalculate defaults, but don't clobber explicit overrides */
            if (!warning_set)  cfg.v_warning  = VCELL_WARNING  * cells;
            if (!low_set)      cfg.v_low      = VCELL_LOW      * cells;
            if (!critical_set) cfg.v_critical = VCELL_CRITICAL * cells;
        } else if (strcmp(argv[i], "--warning") == 0 && i + 1 < argc) {
            cfg.v_warning = atof(argv[++i]);
            warning_set = 1;
        } else if (strcmp(argv[i], "--low") == 0 && i + 1 < argc) {
            cfg.v_low = atof(argv[++i]);
            low_set = 1;
        } else if (strcmp(argv[i], "--critical") == 0 && i + 1 < argc) {
            cfg.v_critical = atof(argv[++i]);
            critical_set = 1;
        } else if (strcmp(argv[i], "--divider") == 0 && i + 1 < argc) {
            cfg.divider = atof(argv[++i]);
            if (cfg.divider <= 0.0f) {
                fprintf(stderr, "Error: --divider must be > 0\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
            cfg.channel = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            cfg.interval_s = atoi(argv[++i]);
            if (cfg.interval_s < 1) {
                fprintf(stderr, "Error: --interval must be >= 1\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--confirm-samples") == 0 && i + 1 < argc) {
            cfg.confirm_samples = atoi(argv[++i]);
            if (cfg.confirm_samples < 2) {
                fprintf(stderr, "Error: --confirm-samples must be >= 2\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--trend-hours") == 0 && i + 1 < argc) {
            float hours = atof(argv[++i]);
            if (hours < 0.0f) {
                fprintf(stderr, "Error: --trend-hours must be >= 0\n");
                return 2;
            }
            cfg.trend_seconds = (int)(hours * 3600.0f);
        } else if (strcmp(argv[i], "--trend-drop") == 0 && i + 1 < argc) {
            cfg.trend_drop = atof(argv[++i]);
            if (cfg.trend_drop < 0.0f) {
                fprintf(stderr, "Error: --trend-drop must be >= 0\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--shutdown-grace") == 0 && i + 1 < argc) {
            cfg.shutdown_grace_s = atoi(argv[++i]);
            if (cfg.shutdown_grace_s < 0) {
                fprintf(stderr, "Error: --shutdown-grace must be >= 0\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (mode == MODE_NONE) {
        fprintf(stderr, "Error: no mode specified "
                        "(--check, --watch, --print, or --calibrate)\n");
        usage(argv[0]);
        return 2;
    }

    /* sanity check threshold ordering */
    if (cfg.v_critical > cfg.v_low || cfg.v_low > cfg.v_warning) {
        fprintf(stderr, "[batt_monitor] Warning: thresholds out of order "
                        "(expected critical < low < warning)\n");
    }

    if (rc_adc_init() < 0) {
        fprintf(stderr, "[batt_monitor] rc_adc_init() failed\n");
        return 0;  /* don't block boot */
    }

    int ret = 0;
    switch (mode) {
        case MODE_CHECK: ret = mode_check(); break;
        case MODE_WATCH: mode_watch();       break;
        case MODE_PRINT: mode_print();       break;
        case MODE_CAL:   mode_calibrate(cal_ref); break;
        default: break;
    }

    rc_adc_cleanup();
    return ret;
}
