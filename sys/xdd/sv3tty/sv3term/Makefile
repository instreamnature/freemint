#
# Makefile for sv3term, a FreeMiNT terminal program for the sv3tty ports.
# Standalone, not part of the FreeMiNT build system.
#

CC      = m68k-atari-mint-gcc
CFLAGS  = -m68020-60 -O2 -Wall -fomit-frame-pointer
LDFLAGS = -s

TARGET  = sv3term
SRCS    = sv3term.c ../charset.c
HDRS    = ../charset.h

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
