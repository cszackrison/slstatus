# See LICENSE file for copyright and license details
# slstatus - suckless status monitor
.POSIX:

include config.mk

REQ = util
COM =\
	components/ai_usage\
	components/battery\
	components/cpu\
	components/datetime\
	components/disk\
	components/entropy\
	components/hostname\
	components/herdr\
	components/ip\
	components/kernel_release\
	components/keyboard_indicators\
	components/load_avg\
	components/microphone\
	components/num_files\
	components/ram\
	components/razer\
	components/run_command\
	components/swap\
	components/temperature\
	components/uptime\
	components/user\
	components/volume\
	components/wifi

all: slstatus

test: all
	sh tests/test_ai_usage.sh
	sh tests/test_opencode_go_cache.sh
	sh tests/test_herdr_status.sh

slstatus: slstatus.o $(COM:=.o) $(REQ:=.o)
slstatus.o: slstatus.c slstatus.h arg.h config.h $(REQ:=.h)
$(COM:=.o): config.mk $(REQ:=.h)

config.h:
	cp config.def.h $@

.o:
	$(CC) -o $@ $(LDFLAGS) $< $(COM:=.o) $(REQ:=.o) $(LDLIBS)

.c.o:
	$(CC) -o $@ -c $(CPPFLAGS) $(CFLAGS) $<

clean:
	rm -f slstatus slstatus.o $(COM:=.o) $(REQ:=.o)

dist:
	rm -rf "slstatus-$(VERSION)"
	mkdir -p "slstatus-$(VERSION)/components"
	cp -R LICENSE Makefile README config.mk config.def.h \
	      arg.h slstatus.c $(COM:=.c) $(REQ:=.c) $(REQ:=.h) \
	      slstatus.1 scripts tests "slstatus-$(VERSION)"
	tar -cf - "slstatus-$(VERSION)" | gzip -c > "slstatus-$(VERSION).tar.gz"
	rm -rf "slstatus-$(VERSION)"

install: all
	mkdir -p "$(DESTDIR)$(PREFIX)/bin"
	cp -f slstatus "$(DESTDIR)$(PREFIX)/bin"
	chmod 755 "$(DESTDIR)$(PREFIX)/bin/slstatus"
	mkdir -p "$(DESTDIR)$(PREFIX)/libexec/slstatus"
	cp -f scripts/claude-usage-cache \
		"$(DESTDIR)$(PREFIX)/libexec/slstatus/claude-usage-cache"
	cp -f scripts/opencode-go-usage-cache \
		"$(DESTDIR)$(PREFIX)/libexec/slstatus/opencode-go-usage-cache"
	cp -f scripts/ai-usage-menu \
		"$(DESTDIR)$(PREFIX)/libexec/slstatus/ai-usage-menu"
	cp -f scripts/calendar-popup \
		"$(DESTDIR)$(PREFIX)/libexec/slstatus/calendar-popup"
	chmod 755 \
		"$(DESTDIR)$(PREFIX)/libexec/slstatus/claude-usage-cache" \
		"$(DESTDIR)$(PREFIX)/libexec/slstatus/opencode-go-usage-cache" \
		"$(DESTDIR)$(PREFIX)/libexec/slstatus/ai-usage-menu" \
		"$(DESTDIR)$(PREFIX)/libexec/slstatus/calendar-popup"
	mkdir -p "$(DESTDIR)$(MANPREFIX)/man1"
	cp -f slstatus.1 "$(DESTDIR)$(MANPREFIX)/man1"
	chmod 644 "$(DESTDIR)$(MANPREFIX)/man1/slstatus.1"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/slstatus"
	rm -f "$(DESTDIR)$(PREFIX)/libexec/slstatus/claude-usage-cache"
	rm -f "$(DESTDIR)$(PREFIX)/libexec/slstatus/opencode-go-usage-cache"
	rm -f "$(DESTDIR)$(PREFIX)/libexec/slstatus/ai-usage-menu"
	rm -f "$(DESTDIR)$(PREFIX)/libexec/slstatus/calendar-popup"
	rm -f "$(DESTDIR)$(MANPREFIX)/man1/slstatus.1"
