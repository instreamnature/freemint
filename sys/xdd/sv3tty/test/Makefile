#
# Makefile for sv3ttest.tos, a TOS test program for sv3tty.
# Standalone, not part of the FreeMiNT build system.
#

CC      = m68k-atari-mint-gcc
CFLAGS  = -m68020-60 -O2 -Wall -fomit-frame-pointer
LDFLAGS = -s

TARGET  = sv3ttest.tos
SRCS    = sv3ttest.c ../sv3tty_proto.c ../tos/cookie.c
HDRS    = ../sv3tty_proto.h ../tos/sv3tty_tos.h

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
