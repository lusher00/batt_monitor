# batt_monitor

`batt_monitor` reads the BeagleBone ADC, classifies the robot battery, and is
the single owner of low-voltage shutdown policy. It publishes an atomic JSON
snapshot at `/run/batt_status.json` for Robot Link, OLED displays, dashboards,
and other consumers.

## Services

Two systemd units are provided:

- `batt_check.service` takes one early boot reading and publishes it. It does
  not shut down because Robot Link and the Pi may not be available yet.
- `batt_monitor.service` watches continuously and owns coordinated runtime
  shutdown.

The watch service enables shutdown by default through
`/etc/default/batt_monitor`. Its default policy is:

- sample every 10 seconds
- require three critical samples
- permit the first critical sample to trigger when voltage has remained below
  the warning threshold for at least two hours and fallen at least 0.30 V
- publish a unique shutdown event
- wait 15 seconds for Robot Link to halt the Pi
- halt the Bone

The RoboClaw retains its own independent undervoltage protection.

## Status interface

Normal status resembles:

```json
{"voltage":10.757,"status":"ok","shutdown_enabled":1,"critical_samples":0,"shutdown_requested":0,"shutdown_event":0,"updated":1790000000}
```

When shutdown commits, `shutdown_requested` becomes `1` and
`shutdown_event` receives a timestamp. `robot-link-boned` forwards each new
event to the Pi exactly once. Consumers must reject a stale status file.

## Build and install

```bash
make
sudo make install
```

Edit `/etc/default/batt_monitor` after installation. The divider must be
calibrated for the actual resistor divider and ADC. Existing configuration is
never overwritten by `make install`.

```bash
sudo systemctl restart batt_monitor
systemctl status batt_check batt_monitor --no-pager
sudo cat /run/batt_status.json
```

## Options

```text
--check                 publish one reading and exit
--watch                 monitor continuously
--print                 print one voltage and exit
--shutdown              enable coordinated shutdown
--interval S            watch interval
--confirm-samples N     critical readings required, minimum 2
--trend-hours H         continuous warning/low duration for fast confirmation
--trend-drop V          required net drop across the trend window
--shutdown-grace S      delay before Bone shutdown
--warning V             warning threshold
--low V                 low threshold
--critical V            critical threshold
--cells N               scale default thresholds
--divider R             calibrated divider ratio
--channel N             ADC channel
```

## Safety

Software shutdown is not hardware undervoltage protection. The Pi currently
receives an orderly Linux shutdown request but the Bone cannot remove Pi power
electrically. Battery and motor hardware protections remain necessary.
