CC = gcc
CFLAGS = -O3 -march=native -fomit-frame-pointer -Wall
LDFLAGS = -lX11

SRC = lvm.c
EXEC = lvm
PREFIX = /usr
DESTDIR =

all: $(EXEC)

$(EXEC): $(SRC)
	$(CC) $(CFLAGS) -o $(EXEC) $(SRC) $(LDFLAGS)

clean:
	rm -f $(EXEC)

install: all
	@echo "Installing lvm to $(DESTDIR)$(PREFIX)/bin..."
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f $(EXEC) $(DESTDIR)$(PREFIX)/bin/
	chmod 755 $(DESTDIR)$(PREFIX)/bin/$(EXEC)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(EXEC)
	@echo "lvm uninstalled."

.PHONY: all clean install uninstall
