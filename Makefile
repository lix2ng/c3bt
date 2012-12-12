all: c3bt

CC = gcc
CFLAGS = -pipe -Wall -Wpadded -std=gnu99 -pedantic -O3
LFLAGS = -lrt

OBJS = c3bt.o c3bt-main.o
c3bt.o: c3bt.c c3bt.h
c3bt-main.o: c3bt-main.c c3bt.h

.c.o:
	$(CC) -c $(CFLAGS) $<

c3bt:	$(OBJS)
	$(CC) -o $@ $^ $(LFLAGS)

clean:
	@rm -f c3bt *.o

# vim: set syn=make noet ts=8 tw=80:
