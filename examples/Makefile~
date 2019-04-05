# Makefile for sw

LDFLAGS =
LOADLIBS =
LDLIBS = ../lib/swsoc_lib.o -I/usr/share/swsoc/include/
APS = status reset getver flush

.c.o:
	$(CC) -Wall -c $*.c -o $@

## Aps

aps : $(APS)

#
clean:
	rm -f *.o *~ $(APS)

