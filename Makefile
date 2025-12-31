CC ?= gcc
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin

CFLAGS += -Wall -Wextra -O2
LIBS = -lX11 -lXinerama

SRC = lwm.c
EXEC = lwm

all: $(EXEC)

$(EXEC): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f $(EXEC)

install:
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(EXEC) $(DESTDIR)$(BINDIR)/$(EXEC)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(EXEC)

.PHONY: all clean install uninstall
