CC      ?= gcc
CFLAGS  := -Wall -Wextra -std=c11 -D_GNU_SOURCE
LDFLAGS := -lpthread

PKG_CFLAGS := $(shell pkg-config --cflags libmicrohttpd libvirt jansson)
PKG_LIBS   := $(shell pkg-config --libs   libmicrohttpd libvirt jansson) -lvirt-qemu

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
	if [ -f systemd/qswm.service ]; then \
		install -Dm644 systemd/qswm.service /etc/systemd/system/qswm.service; \
	fi

test:
	@for t in tests/*.c; do \
		[ -f "$$t" ] || continue; \
		name=$$(basename "$$t" .c); \
		echo "=== Building test: $$name ==="; \
		$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $(BUILDDIR)/$$name $$t $(filter-out $(BUILDDIR)/main.o,$(OBJS)) $(PKG_LIBS) && \
		echo "=== Running test: $$name ===" && \
		./$(BUILDDIR)/$$name; \
	done

.PHONY: all debug clean run install test
