CC = gcc
CFLAGS = -O3 -march=native -Wall -Wextra -Werror -std=gnu89
CPPFLAGS = $(shell pkg-config --cflags libcurl libcjson openssl libpng freetype2 sqlite3)
LDLIBS = $(shell pkg-config --libs libcurl libcjson openssl libpng freetype2 sqlite3) -lm

all: waapi

waapi: waapi.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f waapi

.PHONY: all clean
