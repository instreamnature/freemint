#
# Makefile for the sv3tty TOS driver (sv3tty.prg) and its status tool
# (sv3tstat.tos). Standalone, not part of the FreeMiNT build system.
#

CC      = m68k-atari-mint-gcc
OBJDUMP = m68k-atari-mint-objdump
CFLAGS  = -m68020-60 -O2 -Wall -fomit-frame-pointer
LDFLAGS = -s

PROTO   = ../sv3tty_proto.c ../sv3tty_proto.h

all: sv3tty.prg sv3tstat.tos sv3tclos.ttp sv3taux.ttp

sv3tty.prg: sv3tty_tos.c timerc.S bios.S cookie.c sv3tty_tos.h $(PROTO)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ sv3tty_tos.c timerc.S bios.S cookie.c ../sv3tty_proto.c

sv3tstat.tos: sv3tstat.c cookie.c sv3tty_tos.h ../sv3tty_proto.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ sv3tstat.c cookie.c

sv3tclos.ttp: sv3tclos.c cookie.c sv3tty_tos.h ../sv3tty_proto.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ sv3tclos.c cookie.c

sv3taux.ttp: sv3taux.c cookie.c sv3tty_tos.h ../sv3tty_proto.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ sv3taux.c cookie.c

# Check that the code called from the interrupt uses no FPU instructions
fpucheck:
	$(CC) $(CFLAGS) -o sv3tty.sym sv3tty_tos.c timerc.S bios.S cookie.c ../sv3tty_proto.c
	$(OBJDUMP) -d sv3tty.sym | grep -c -E '\sf(move|add|sub|mul|div|cmp|tst|save|restore)' || true
	rm -f sv3tty.sym

clean:
	rm -f sv3tty.prg sv3tstat.tos sv3tclos.ttp sv3taux.ttp sv3tty.sym

.PHONY: all clean fpucheck
