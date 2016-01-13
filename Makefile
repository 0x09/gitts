CC ?= cc
CFLAGS := -Os -std=c99 $(CFLAGS)
LIBS = -lsqlite3 -lgit2
PREFIX ?= /usr/local

OS := $(shell uname)
ifeq ($(OS), Darwin)
	FLAGS =-DHAVE_BIRTHTIME
else ifeq ($(OS), FreeBSD)
	FLAGS =-DHAVE_BIRTHTIME
else
	FLAGS =-D_GNU_SOURCE
endif

all: gitts

gitts: gitts.c
	$(CC) $(CFLAGS) $(FLAGS) -o $@ $< $(LIBS)
	
install: gitts
	install $< $(PREFIX)/bin/

clean:
	rm -f gitts

.PHONY: all install clean
