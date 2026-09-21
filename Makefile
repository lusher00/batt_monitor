CC      = gcc
CFLAGS  = -Wall -O2
LIBS    =
TARGET  = batt_monitor
PREFIX  = /usr/local/bin

$(TARGET): batt_monitor.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

install: $(TARGET)
	install -m 755 $(TARGET) $(PREFIX)/$(TARGET)
	install -m 644 batt_check.service   /etc/systemd/system/
	install -m 644 batt_monitor.service /etc/systemd/system/
	test -e /etc/default/batt_monitor || install -m 644 batt_monitor.env.example /etc/default/batt_monitor
	systemctl daemon-reload
	systemctl enable batt_check.service
	systemctl enable batt_monitor.service
	systemctl start  batt_monitor.service
	@echo "Installed. Boot check + watch service enabled."

uninstall:
	systemctl disable batt_check.service batt_monitor.service 2>/dev/null || true
	systemctl stop   batt_monitor.service 2>/dev/null || true
	rm -f $(PREFIX)/$(TARGET)
	rm -f /etc/systemd/system/batt_check.service
	rm -f /etc/systemd/system/batt_monitor.service
	systemctl daemon-reload

test:
	$(CC) $(CFLAGS) -o tests/test_policy tests/test_policy.c
	./tests/test_policy

.PHONY: install uninstall test
