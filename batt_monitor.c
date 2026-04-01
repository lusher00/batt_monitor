/**
 * batt_monitor.c
 *
 * Reads LiPo voltage via a resistor divider on AIN1.
 * Divider: 68k (top) + 10k (bottom) → Vbat/7.8 on AIN1.
 * ADC ref = 1.8V, 12-bit (0–4095).
 *
 * Modes:
 *   --check     single shot, logs voltage + status, exits 0 always
 *   --watch     loop (default 60s), logs warnings
 *   --print     print raw voltage to stdout as "12.34" and exit (scripting)
 *
 * Threshold flags (volts):
 *   --warning V     default 10.5  (3.50V/cell × 3S)
 *   --low V         default  9.9  (3.30V/cell × 3S)
 *   --critical V    default  9.6  (3.20V/cell × 3S)
 *
 * Hardware flags:
 *   --cells N       set cell count (2–6); scales all default thresholds
 *   --divider R     override resistor divider ratio (default 7.8)
 *   --channel N     override ADC channel (default 1)
 *
 * Timing flags:
 *   --interval S    watch loop interval in seconds (default 60)
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <robotcontrol.h>

/* ── defaults ────────────────────────────────────────────────────────── */
#define DEFAULT_R_TOP          68000.0f
#define DEFAULT_R_BOT          10000.0f
#define DEFAULT_DIVIDER_RATIO  ((DEFAULT_R_TOP + DEFAULT_R_BOT) / DEFAULT_R_BOT)  /* 7.8 */
#define DEFAULT_ADC_CHANNEL    1
#define DEFAULT_WATCH_INTERVAL 60

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
    int   shutdown_enabled;  /* 0 = warn only (default), 1 = shutdown at critical */
} Config;

static Config cfg;

/* ── helpers ─────────────────────────────────────────────────────────── */

static void write_status(float vbat, const char *status)
{
    FILE *f = fopen("/run/batt_status.json", "w");
    if (!f) return;
    fprintf(f, "{\"voltage\":%.3f,\"status\":\"%s\",\"shutdown_enabled\":%d}\n",
            vbat, status, cfg.shutdown_enabled);
    fclose(f);
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

static void do_shutdown(float vbat)
{
    log_msg("CRITICAL — shutting down", vbat);
    sync();
    sleep(2);
    system("shutdown -h now 'Battery critical'");
}

/* ── modes ───────────────────────────────────────────────────────────── */

static int mode_check(void)
{
    float vbat = read_vbat();
    if (vbat < 0.0f) {
        fprintf(stderr, "[batt_monitor] ADC error — skipping\n");
        return 0;
    }

    const char *log_label;
    const char *status = classify(vbat, &log_label);
    write_status(vbat, status);
    log_msg(log_label, vbat);

    if (cfg.shutdown_enabled && vbat <= cfg.v_critical)
        do_shutdown(vbat);

    return 0;  /* always 0 — don't let systemd abort boot */
}

static void mode_watch(void)
{
    /* initial reading at start */
    float vbat0 = read_vbat();
    if (vbat0 >= 0.0f) {
        const char *log_label;
        const char *status = classify(vbat0, &log_label);
        write_status(vbat0, status);
        log_msg(log_label, vbat0);
        if (cfg.shutdown_enabled && vbat0 <= cfg.v_critical) {
            do_shutdown(vbat0);
            return;
        }
    }

    while (1) {
        sleep(cfg.interval_s);

        float vbat = read_vbat();
        if (vbat < 0.0f) continue;  /* transient ADC error, keep going */

        const char *log_label;
        const char *status = classify(vbat, &log_label);
        write_status(vbat, status);
        log_msg(log_label, vbat);

        if (cfg.shutdown_enabled && vbat <= cfg.v_critical) {
            do_shutdown(vbat);
            return;
        }
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
        "\n"
        "Safety options:\n"
        "  --shutdown       Enable graceful shutdown at critical voltage (default: OFF)\n"
        "\n"
        "Examples:\n"
        "  %s --check\n"
        "  %s --watch --shutdown\n"
        "  %s --watch --interval 30 --warning 10.8 --critical 9.9\n"
        "  %s --cells 4 --watch --shutdown\n"
        "  %s --print\n",
        prog,
        VCELL_WARNING  * DEFAULT_CELLS,
        VCELL_LOW      * DEFAULT_CELLS,
        VCELL_CRITICAL * DEFAULT_CELLS,
        DEFAULT_CELLS,
        DEFAULT_DIVIDER_RATIO,
        DEFAULT_ADC_CHANNEL,
        DEFAULT_WATCH_INTERVAL,
        prog, prog, prog, prog, prog);
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

    /* track explicit overrides so --cells doesn't clobber them */
    int warning_set  = 0;
    int low_set      = 0;
    int critical_set = 0;

    enum { MODE_NONE, MODE_CHECK, MODE_WATCH, MODE_PRINT } mode = MODE_NONE;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) {
            mode = MODE_CHECK;
        } else if (strcmp(argv[i], "--watch") == 0) {
            mode = MODE_WATCH;
        } else if (strcmp(argv[i], "--print") == 0) {
            mode = MODE_PRINT;
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
        fprintf(stderr, "Error: no mode specified (--check, --watch, or --print)\n");
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
        default: break;
    }

    rc_adc_cleanup();
    return ret;
}