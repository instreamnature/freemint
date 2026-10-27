/*
 * sv3tty_tos.h
 *
 * Shared between the sv3tty TOS driver (TSR) and its tools: the cookie the
 * driver installs, and what it points to.
 */

#ifndef SV3TTY_TOS_H
#define SV3TTY_TOS_H

#include "../sv3tty_proto.h"

#define SV3T_COOKIE			0x53563354L		/* 'SV3T': our cookie and XBRA id */
#define SUPV_COOKIE			0x53757056L		/* 'SupV': installed by sv_xbios.prg */
#define SV3T_INFO_VERSION	4
#define RSVF_COOKIE			0x52535646L		/* 'RSVF': port list for terminal programs */
#define SV3T_MAX_PORTS		16

/* SuperVidel firmware version register. 300..399 is a SuperVidel 3, which
 * has the mailboxes. Only read it when the SupV cookie exists. */
#define SV3T_SV_VERSION		(*(volatile unsigned long *)0x8001007CUL)

/* The cookie value points to this */
struct sv3t_info
{
	unsigned long	magic;			/* SV3T_COOKIE */
	unsigned short	version;		/* SV3T_INFO_VERSION */
	unsigned short	nports;
	unsigned short	sv_firmware;	/* SuperVidel firmware version */
	unsigned short	tick_budget;	/* Messages per timer tick, each way */
	struct sv3p		*proto;			/* Protocol state, see sv3tty_proto.h */
	volatile unsigned long ticks;	/* Timer ticks handled */
	unsigned short	first_dev;		/* BIOS device number of port 0 */
	unsigned short	old_maptab;		/* Bconmap rows before ours (0 with DRVIN) */

	/* Close a port: CLOSE to Linux, which hangs up the session there. The
	 * next access opens the port again. Supervisor mode only (it turns
	 * interrupts off for a moment). Returns 0, or -1 for a bad port. */
	long			(*close_port)(long port);

	unsigned char	rsvf_ok;		/* Ports are in the RSVF list */
	unsigned char	drvin;			/* Installed through HSMODEM's DRVIN.PRG */
	unsigned char	gemdos_ports;	/* Ports also in U:\DEV\ (with DRVIN) */
	unsigned char	nbios;			/* Ports 0..nbios-1 have a BIOS device */

	/* BIOS device number of each port, 0 = none. With DRVIN they are given
	 * out by DRVIN, whose table has room for devices 6..15 only, so not
	 * every port may get one; those ports are GEMDOS devices only. */
	unsigned char	bdev[SV3T_MAX_PORTS];

	unsigned char	rsvf_ports;		/* Ports listed in RSVF */
	unsigned char	res2[3];
};

/* Cookie jar access, supervisor mode only */
int sv3t_get_cookie(long id, long *value);

/* Wait for a key, at most 'seconds' (0 = no limit). User mode. */
void sv3t_wait_key(int seconds);

/* How long the driver waits at the end of its start, so its messages can
 * be read when it is started from the desktop. Short, so a start from the
 * AUTO folder is hardly delayed. */
#define SV3T_START_WAIT		3

#endif /* SV3TTY_TOS_H */
