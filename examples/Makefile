# Makefile for sw

LDFLAGS =
LOADLIBS =
LDLIBS = ../lib/swpci_lib.o -I../include/
APS = status reset getver flush

.c.o:
	$(CC) -Wall -c $*.c -o $@

## Aps

aps : $(APS)

#
clean:
	rm -f *.o *~ $(APS)

