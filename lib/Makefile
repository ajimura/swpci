# Makefile for sw

LDFLAGS = 
LOADLIBS =
LDLIBS = 
APS = 

libs: swpci_lib.o

swpci_lib.o:  swpci_lib.c ../include/swpci_lib.h
	$(CC) -Wall -I../include/ -c swpci_lib.c

clean:
	rm -f *.o *~

