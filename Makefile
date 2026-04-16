CC      ?= gcc
CFLAGS  := -Wall -Wextra -std=c11 -D_GNU_SOURCE
LDFLAGS := -lpthread

PKG_CFLAGS := $(shell pkg-config --cflags libmicrohttpd libvirt jansson libsystemd)
PKG_LIBS   := $(shell pkg-config --libs   libmicrohttpd libvirt jansson libsystemd) -lvirt-qemu

SRCDIR  := src
BUILDDIR:= build
TARGET  := $(BUILDDIR)/qswm

SRCS    := $(wildcard $(SRCDIR)/*.c)
OBJS    := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))
DEPS    := $(OBJS:.o=.d)

# Default: release build
all: CFLAGS += -O2
all: $(TARGET)

# Debug build
debug: CFLAGS += -g -O0 -DDEBUG
debug: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(PKG_LIBS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR)

run: all
	./$(TARGET) --port 9091 --static-dir ./static

install: all
	install -Dm755 $(TARGET) /usr/local/bin/qswm
	install -d /usr/local/share/qswm/static
	cp -r static/* /usr/local/share/qswm/static/ 2>/dev/null || true
	install -d /etc/qswm/conf.d
	install -Dm755 scripts/idle-check.sh /usr/local/libexec/qswm/idle-check.sh
	install -Dm644 systemd/qswm.socket /etc/systemd/system/qswm.socket
	install -Dm644 systemd/qswm.service /etc/systemd/system/qswm.service
	install -Dm644 systemd/qswm-idle.timer /etc/systemd/system/qswm-idle.timer
	install -Dm644 systemd/qswm-idle.service /etc/systemd/system/qswm-idle.service
	systemctl daemon-reload
	@echo ""
	@echo "Installed. Enable socket activation with:"
	@echo "  sudo systemctl enable --now qswm.socket qswm-idle.timer"

uninstall:
	systemctl disable --now qswm.socket qswm.service qswm-idle.timer 2>/dev/null || true
	rm -f /usr/local/bin/qswm
	rm -rf /usr/local/share/qswm
	rm -f /usr/local/libexec/qswm/idle-check.sh
	rmdir /usr/local/libexec/qswm 2>/dev/null || true
	rm -f /etc/systemd/system/qswm.socket
	rm -f /etc/systemd/system/qswm.service
	rm -f /etc/systemd/system/qswm-idle.timer
	rm -f /etc/systemd/system/qswm-idle-check.service
	rm -f /etc/systemd/system/qswm-idle.service
	systemctl daemon-reload
	@echo "Uninstalled."

test:
	@for t in tests/*.c; do \
		[ -f "$$t" ] || continue; \
		name=$$(basename "$$t" .c); \
		echo "=== Building test: $$name ==="; \
		$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $(BUILDDIR)/$$name $$t $(filter-out $(BUILDDIR)/main.o,$(OBJS)) $(PKG_LIBS) && \
		echo "=== Running test: $$name ===" && \
		./$(BUILDDIR)/$$name; \
	done

.PHONY: all debug clean run install uninstall test
