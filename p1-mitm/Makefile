# p1-mitm Makefile
# Target: Raspberry Pi 4B (arm64), Debian Trixie
# No external library dependencies — pure libc only
# Build: make
# Install: sudo make install

CC      := gcc
TARGET  := p1-mitm
SRCDIR  := src
SRCS    := $(wildcard $(SRCDIR)/*.c)
OBJS    := $(SRCS:.c=.o)

CFLAGS  := -Wall -Wextra -O2 -g -std=gnu11 \
           -D_GNU_SOURCE
LDFLAGS :=   # No external libs — pure libc

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo ""
	@echo "Build OK: $(TARGET)"
	@echo "Install:  sudo make install"

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(SRCDIR)/*.o $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
	install -m 644 deploy/p1-mitm.service /etc/systemd/system/
	systemctl daemon-reload
	@echo "Installed. Enable with: sudo systemctl enable --now p1-mitm"

uninstall:
	systemctl stop p1-mitm 2>/dev/null || true
	systemctl disable p1-mitm 2>/dev/null || true
	rm -f /usr/local/bin/$(TARGET)
	rm -f /etc/systemd/system/p1-mitm.service
	systemctl daemon-reload
