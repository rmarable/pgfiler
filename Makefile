## Copyright (c) 2025 by Paul Vixie
##
## Permission to use, copy, modify, and distribute this software for any
## purpose with or without fee is hereby granted, provided that the above
## copyright notice and this permission notice appear in all copies.
##
## THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
## ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
## OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
## CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
## DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
## PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
## ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
## SOFTWARE.

CC= clang
CBUILD= -W -Wall -Wcast-qual -Wpointer-arith -Wwrite-strings \
	-Wmissing-prototypes  -Wbad-function-cast -Wnested-externs \
	-Wunused -Wshadow -Wmissing-noreturn -Wswitch-enum \
	-Wformat-nonliteral -Werror
ALL= pgfiler

CDEBUG= -g
LDFLAGS= -L/usr/lib/postgresql
CFLAGS= -I/usr/include/pgsql $(CDEBUG) $(CBUILD)
LIBS= -lpq

all: $(ALL)

clean:; rm -f $(ALL); rm -f *.o

pgfiler: pgfiler.o Makefile
	$(CC) $(LDFLAGS) -o pgfiler pgfiler.o $(LIBS)

pgfiler.o: pgfiler.c Makefile
