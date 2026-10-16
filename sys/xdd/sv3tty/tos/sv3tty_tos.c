/*
 * sv3tty_tos.c
 *
 * sv3tty driver for plain TOS, a TSR for the AUTO folder: virtual serial
 * ports to PetaLinux on SuperVidel 3 over mailbox channel 1, see
 * PROTOCOL.md and sv3tty_proto.c.
 *
 * Step 3 of the driver: the ports are BIOS devices. They are added to the
 * end of the Bconmap table, so on a Falcon (devices 6..8 = MFP, SCC B,
 * SCC A) port 0 is BIOS device 9. Programs use them with Bconin, Bconout,
 * Bconstat, Bcostat and Iorec, or as AUX: after Bconmap(dev).
 *
 * Install:
 *  - Needs the SupV cookie from sv_xbios.prg (so it must come after
 *    sv_xbios.prg in the AUTO folder) and SuperVidel firmware 300..399.
 *  - Installs the cookie 'SV3T', which points to struct sv3t_info. Tools
 *    (sv3tstat.tos) use it, and it keeps the driver from being installed
 *    twice. sv3ttest.tos only runs with the driver in its -b mode.
 *  - Extends the Bconmap table with one row per port.
 *  - Hooks Timer C (vector 0x114, XBRA id 'SV3T') and stays resident.
 *
 * Interrupt:
 *  - Each Timer C tick calls sv3p_poll. The tick runs at IPL 6, so the
 *    work per tick is limited (tick_budget messages each way); the rest is
 *    done at the next tick. When idle, a tick reads one mailbox status
 *    register.
 *  - Only the interrupt touches the mailbox. The BIOS functions only use
 *    the port buffers (IORECs).
 *
 * BIOS functions (called through bios.S, in the context of the calling
 * program):
 *  - The first call for a port opens it (OPEN to Linux). There is no close
 *    in the BIOS; that comes in the next step.
 *  - Bconout waits while the transmit buffer is full. When the link to
 *    Linux is down, characters are thrown away, so a program does not hang
 *    when Linux is not running.
 */

#include <stdio.h>
#include <osbind.h>

#include "sv3tty_tos.h"


#define NPORTS				16
#define BUFSIZE				1024		/* Per port and direction */
#define TICK_BUDGET			16			/* Messages per tick, each way */
#define MAX_OLD_MAPTAB		32			/* Bconmap rows of other drivers we can keep */

#define TIMERC_VEC			(*(void **)0x114L)	/* Timer C, 200 Hz */
#define MCH_COOKIE			0x5F4D4348L			/* '_MCH', machine type */

/* Error codes from install() */
#define ERR_INSTALLED		1
#define ERR_NO_SUPV			2
#define ERR_NOT_SV3			3
#define ERR_COOKIE_JAR		4
#define ERR_NO_BCONMAP		5
#define ERR_MAPTAB_SIZE		6

/* One row of the Bconmap table: the BIOS functions of a device */
typedef struct
{
	long			bconstat;
	long			bconin;
	long			bcostat;
	long			bconout;
	long			rsconf;
	SV3P_IOREC		*iorec;		/* Receive IOREC, the transmit IOREC follows it */
} MAPTAB;

/* What Bconmap(-2) returns */
typedef struct
{
	MAPTAB			*maptab;
	short			maptabsize;	/* Rows; row 0 is BIOS device 6 */
} BCONMAP;

/* mintlib: memory used by the program, for Ptermres */
extern long _PgmSize;

/* timerc.S */
extern void sv3t_timerc(void);
extern void *sv3t_old_timerc;

/* bios.S */
extern void sv3t_bconstat_asm(void);
extern void sv3t_bconin_asm(void);
extern void sv3t_bcostat_asm(void);
extern void sv3t_bconout_asm(void);
extern void sv3t_rsconf_asm(void);

static struct sv3p			proto;
static struct sv3p_port		ports[NPORTS];
static unsigned char		bufs[NPORTS][2][BUFSIZE];
static struct sv3t_info		info;
static unsigned short		sv_firmware;
static unsigned short		first_dev;		/* BIOS device number of port 0 */
static MAPTAB				new_maptab[MAX_OLD_MAPTAB + NPORTS];



/// @brief		Raise the interrupt level to 7 (no interrupts)
/// @return		The old status register, for ipl_restore
static inline unsigned short ipl7(void)
{
	unsigned short sr;

	__asm__ volatile ("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "memory");
	return sr;
}


/// @brief		Restore the interrupt level
/// @param sr	Status register from ipl7
static inline void ipl_restore(unsigned short sr)
{
	__asm__ volatile ("move.w %0,%%sr" : : "d"(sr) : "memory");
}


/// @brief		Called from the Timer C interrupt (timerc.S), 200 times a
///				second, at IPL 6. Must stay short; sv3p_poll limits itself
///				to tick_budget messages each way.
void sv3t_tick(void)
{
	info.ticks++;
	sv3p_poll(&proto);
}



/* ================================================================== */
/* BIOS functions, called through bios.S                              */
/* ================================================================== */

/// @brief		Port number for a BIOS device number, and open the port if it
///				is not open yet (the BIOS has no open, so the first access
///				opens it). Interrupts are off while the port is set up, since
///				the timer interrupt reads the port state.
/// @param dev	BIOS device number
/// @return		Port number, or -1 if the device is not one of ours
static int dev_port(long dev)
{
	unsigned long i = (unsigned long)(dev - first_dev);
	unsigned short sr;

	if (i >= NPORTS)
		return -1;

	if (!ports[i].open)
	{
		sr = ipl7();
		if (!ports[i].open)
			sv3p_open(&proto, (unsigned short)i, bufs[i][0], BUFSIZE, bufs[i][1], BUFSIZE, 0, 0);
		ipl_restore(sr);
	}
	return (int)i;
}


/// @brief		Bconstat: is a character waiting?
/// @param dev	BIOS device number
/// @return		-1 if a character is waiting, 0 otherwise
long sv3t_bconstat(long dev)
{
	int i = dev_port(dev);

	if (i < 0)
		return 0;
	return sv3p_rx_count(&proto, i) ? -1 : 0;
}


/// @brief		Bconin: wait for a character and return it
/// @param dev	BIOS device number
/// @return		The character (0..255)
long sv3t_bconin(long dev)
{
	int i = dev_port(dev);
	unsigned char c;
	unsigned short sr;
	long n;

	if (i < 0)
		return 0;

	for (;;)
	{
		// sv3p_read also updates the credit to return to Linux, which the
		// timer interrupt sends and clears, so interrupts are off briefly
		sr = ipl7();
		n = sv3p_read(&proto, i, &c, 1);
		ipl_restore(sr);
		if (n)
			return c;
	}
}


/// @brief		Bcostat: can a character be sent?
/// @param dev	BIOS device number
/// @return		-1 if there is room (or the link is down, then Bconout
///				throws the character away), 0 otherwise
long sv3t_bcostat(long dev)
{
	int i = dev_port(dev);

	if (i < 0)
		return 0;
	if (!proto.link_up)
		return -1;
	return sv3p_tx_room(&proto, i) ? -1 : 0;
}


/// @brief		Bconout: send a character. Waits while the transmit buffer is
///				full; the timer interrupt empties it. With the link down the
///				character is thrown away.
/// @param dev	BIOS device number
/// @param c	Character
/// @return		-1
long sv3t_bconout(long dev, long c)
{
	int i = dev_port(dev);
	unsigned char b = (unsigned char)c;

	if (i < 0)
		return 0;

	while (!sv3p_write(&proto, i, &b, 1))
	{
		if (!proto.link_up)
			break;
	}
	return -1;
}



/* ================================================================== */
/* Installation                                                       */
/* ================================================================== */

/// @brief			Add a cookie to the cookie jar. Supervisor mode, with
///					interrupts off.
/// @param id		Cookie id
/// @param value	Cookie value
/// @return			1 on success, 0 if there is no jar or it is full
static int add_cookie(long id, long value)
{
	long *c = *(long **)0x5a0L;
	long n = 0;

	if (!c)
		return 0;

	// Find the end marker. Its value is the size of the jar in entries.
	while (c[n * 2])
		n++;
	if (n + 1 >= c[n * 2 + 1])
		return 0;

	// New end marker one entry further, then our cookie in place of the old
	c[n * 2 + 2] = 0;
	c[n * 2 + 3] = c[n * 2 + 1];
	c[n * 2 + 1] = value;
	c[n * 2]     = id;
	return 1;
}


/// @brief		Add our ports to the end of the Bconmap table. A new, larger
///				table is built in our memory with the old rows copied; the
///				old table is left as it is, since others may point into it.
/// @return		0, or an ERR_ code
static long install_bconmap(void)
{
	BCONMAP *bm;
	long mch = 0;
	short old;
	unsigned short sr;
	int i;

	// Bconmap(0) returns 0 when Bconmap exists
	if (Bconmap(0) != 0)
		return ERR_NO_BCONMAP;
	bm = (BCONMAP *)Bconmap(-2);

	// Falcon TOS reports 1 row, but has 3 (MFP, SCC B, SCC A). FreeMiNT
	// corrects it the same way (init.c). Another size means another driver
	// has already extended the table, and then it is right.
	old = bm->maptabsize;
	sv3t_get_cookie(MCH_COOKIE, &mch);
	if (old == 1 && (mch >> 16) == 3)
		old = 3;
	if (old < 0 || old > MAX_OLD_MAPTAB)
		return ERR_MAPTAB_SIZE;

	for (i = 0; i < old; i++)
		new_maptab[i] = bm->maptab[i];

	for (i = 0; i < NPORTS; i++)
	{
		MAPTAB *m = &new_maptab[old + i];

		m->bconstat = (long)sv3t_bconstat_asm;
		m->bconin   = (long)sv3t_bconin_asm;
		m->bcostat  = (long)sv3t_bcostat_asm;
		m->bconout  = (long)sv3t_bconout_asm;
		m->rsconf   = (long)sv3t_rsconf_asm;
		m->iorec    = SV3P_RX(&ports[i]);	// io[0], io[1] follows
	}

	// The IORECs must be valid before a program can see them through
	// Iorec, even before the port is opened
	for (i = 0; i < NPORTS; i++)
	{
		SV3P_IOREC *r = SV3P_RX(&ports[i]);

		r[0].bufaddr = bufs[i][0];
		r[0].buflen  = BUFSIZE;
		r[1].bufaddr = bufs[i][1];
		r[1].buflen  = BUFSIZE;
	}

	first_dev       = 6 + old;
	info.first_dev  = first_dev;
	info.old_maptab = old;

	sr = ipl7();
	bm->maptab     = new_maptab;
	bm->maptabsize = old + NPORTS;
	ipl_restore(sr);

	return 0;
}


/// @brief		Install the driver. Called through Supexec, since the cookie
///				jar, the SuperVidel registers and the vectors need supervisor
///				mode.
/// @return		0, or an ERR_ code
static long install(void)
{
	unsigned short sr;
	long r;
	int ok;

	if (sv3t_get_cookie(SV3T_COOKIE, 0))
		return ERR_INSTALLED;

	// The SupV cookie tells that a SuperVidel is there, so its registers
	// can be read without a bus error
	if (!sv3t_get_cookie(SUPV_COOKIE, 0))
		return ERR_NO_SUPV;
	sv_firmware = (unsigned short)(SV3T_SV_VERSION & 0x3FF);
	if (sv_firmware < 300 || sv_firmware > 399)
		return ERR_NOT_SV3;

	// Protocol state, with a small budget per timer tick
	sv3p_init(&proto, ports, NPORTS);
	proto.rx_budget = TICK_BUDGET;
	proto.tx_budget = TICK_BUDGET;

	info.magic       = SV3T_COOKIE;
	info.version     = SV3T_INFO_VERSION;
	info.nports      = NPORTS;
	info.sv_firmware = sv_firmware;
	info.tick_budget = TICK_BUDGET;
	info.proto       = &proto;
	info.ticks       = 0;

	// The ports as BIOS devices. Before the cookie, so a failure leaves
	// nothing behind.
	r = install_bconmap();
	if (r)
		return r;

	sr = ipl7();
	ok = add_cookie(SV3T_COOKIE, (long)&info);
	ipl_restore(sr);
	if (!ok)
		return ERR_COOKIE_JAR;

	// Start the resync before the timer is hooked, so nothing runs twice
	sv3p_start(&proto);

	// Hook Timer C. The old handler must be stored before ours can run, so
	// both are done with interrupts off. (Setexc does the same under TOS.)
	sr = ipl7();
	sv3t_old_timerc = TIMERC_VEC;
	TIMERC_VEC      = (void *)sv3t_timerc;
	ipl_restore(sr);

	return 0;
}


int main(void)
{
	long r;
	char line[100];

	(void)Cconws("SV3TTY driver for TOS (step 3: BIOS devices)\r\n");

	r = Supexec(install);
	switch (r)
	{
	case 0:
		break;
	case ERR_INSTALLED:
		(void)Cconws("sv3tty: already installed\r\n");
		break;
	case ERR_NO_SUPV:
		(void)Cconws("sv3tty: no SupV cookie, sv_xbios.prg must be loaded first. Not installed.\r\n");
		break;
	case ERR_NOT_SV3:
		sprintf(line, "sv3tty: SuperVidel firmware %u is not a SuperVidel 3. Not installed.\r\n",
				sv_firmware);
		(void)Cconws(line);
		break;
	case ERR_NO_BCONMAP:
		(void)Cconws("sv3tty: this TOS has no Bconmap. Not installed.\r\n");
		break;
	case ERR_MAPTAB_SIZE:
		(void)Cconws("sv3tty: the Bconmap table is too large. Not installed.\r\n");
		break;
	default:
		(void)Cconws("sv3tty: cookie jar full. Not installed.\r\n");
		break;
	}
	if (r)
	{
		sv3t_wait_key(SV3T_START_WAIT);
		return 1;
	}

	sprintf(line, "sv3tty: SuperVidel firmware %u, %d ports = BIOS devices %u..%u\r\n",
			sv_firmware, NPORTS, first_dev, first_dev + NPORTS - 1);
	(void)Cconws(line);
	(void)Cconws("sv3tty: Timer C hooked, waiting for link\r\n");

	// Time to read the messages when started from the desktop. The timer
	// is already hooked, so the link comes up meanwhile.
	sv3t_wait_key(SV3T_START_WAIT);

	Ptermres(_PgmSize, 0);
	return 0;
}
