#
# Makefile for the rpp tool. To be compiled with either gcc or clang
#

CFLAGS = -O3 -s -std=gnu89 -Wall -Wextra -pedantic -Wformat-security -Werror -Wstrict-prototypes
CLIBS = -lresolv
CC = gcc

all: rpp README

rpp: rpp.c
	$(CC) rpp.c $(CLIBS) -o rpp $(CFLAGS)

README: rpp
	./rpp --help > README

clean:
	rm -f *.o rpp
