PKGS := gio-2.0 libpipewire-0.3 libavcodec libavutil libswscale

CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lpthread

OBJS := main.o screencast.o encoder.o

vscreen-send: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c protocol.h screencast.h encoder.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: vscreen-send
	install -Dm755 vscreen-send $(DESTDIR)/usr/local/bin/vscreen-send

clean:
	rm -f vscreen-send $(OBJS)

.PHONY: install clean
