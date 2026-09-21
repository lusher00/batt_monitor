# batt_monitor — build on the BeagleBone, driven from the Mac
#
# Local (run on the bone):
#   make                 build
#   sudo make install    install binary + services, enable at boot
#   sudo make uninstall
#
# Remote (run on the Mac):
#   make sync            rsync sources to the bone
#   make remote          sync + build on the bone
#   make remote-install  sync + build + install on the bone
#   make log             tail the battery log
#   make status          show service state + /run/batt_status.json

CC      = gcc
CFLAGS  = -Wall -O2
LIBS    =
TARGET  = batt_monitor
PREFIX  = /usr/local/bin

# ── remote settings (override on the command line if needed) ─────────
BONE_USER ?= debian
BONE_HOST ?= boneblue-0
BONE_DIR  ?= ~/batt_monitor
BONE      := $(BONE_USER)@$(BONE_HOST)

RSYNC_EXCLUDES = --exclude '.git' --exclude '.vscode' --exclude '$(TARGET)' \
                 --exclude '*.o' --exclude 'tests/test_policy' --exclude '.DS_Store'

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

# ── remote (invoked from the Mac) ────────────────────────────────────
sync:
	@ssh $(BONE) 'mkdir -p $(BONE_DIR)'
	rsync -az --delete $(RSYNC_EXCLUDES) ./ $(BONE):$(BONE_DIR)/

remote: sync
	@ssh $(BONE) 'cd $(BONE_DIR) && make'

remote-install: sync
	@ssh -t $(BONE) 'cd $(BONE_DIR) && make && sudo make install'

remote-clean:
	@ssh $(BONE) 'cd $(BONE_DIR) && make clean'

# quick read without installing
print: sync
	@ssh $(BONE) 'cd $(BONE_DIR) && make -s && ./$(TARGET) --print'

log:
	@ssh $(BONE) 'tail -f /var/log/batt_monitor.log'

status:
	@ssh $(BONE) 'systemctl --no-pager status batt_monitor.service; \
	              echo; cat /run/batt_status.json 2>/dev/null || echo "(no status file)"'

.PHONY: install uninstall test sync remote remote-install remote-clean print log status
