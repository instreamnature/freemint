/*
 * sv3tty_tos.c
 *
 * sv3tty driver for plain TOS, a TSR for the AUTO folder: virtual serial
 * ports to PetaLinux on SuperVidel 3 over mailbox channel 1, see
 * PROTOCOL.md and sv3tty_proto.c.
 *
 * Step 2 of the driver: it installs itself, hooks Timer C (200 Hz) and keeps
 * the link to Linux up. The ports are not yet BIOS devices (Bconmap), that
 * is the next step.
 *
 * Install:
 *  - Needs the SupV cookie from sv_xbios.prg (so it must come after
 *    sv_xbios.prg in the AUTO folder) and SuperVidel firmware 300..399.
 *  - Installs the cookie 'SV3T', which points to struct sv3t_info. Tools
 *    (sv3tstat.tos) use it, and it keeps the driver from being installed
 *    twice. sv3ttest.tos refuses to run when it exists, since both would
 *    use the same mailbox.
 *  - Hooks Timer C (vector 0x114, XBRA id 'SV3T') and stays resident.
 *
 * Interrupt:
 *  - Each Timer C tick calls sv3p_poll. The tick runs at IPL 6, so the
 *    work per tick is limited (tick_budget messages each way); the rest is
 *    done at the next tick. When idle, a tick reads one mailbox status
 *    register.
 *  - Only the interrupt touches the mailbox. Programs (later through the
 *    BIOS functions) only use the port buffers.
 */

#include <stdio.h>
#include <osbind.h>

#include "sv3tty_tos.h"


#define NPORTS				16
#define BUFSIZE				1024		/* Per port and direction */
#define TICK_BUDGET			16			/* Messages per tick, each way */

#define TIMERC_VEC			(*(void **)0x114L)	/* Timer C, 200 Hz */

/* Error codes from install() */
#define ERR_INSTALLED		1
#define ERR_NO_SUPV			2
#define ERR_NOT_SV3			3
#define ERR_COOKIE_JAR		4

/* mintlib: memory used by the program, for Ptermres */
extern long _PgmSize;

/* timerc.S */
extern void sv3t_timerc(void);
extern void *sv3t_old_timerc;

static struct sv3p			proto;
static struct sv3p_port		ports[NPORTS];
static unsigned char		bufs[NPORTS][2][BUFSIZE];	/* Used from step 3 */
static struct sv3t_info		info;
static unsigned short		sv_firmware;


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


/// @brief		Install the driver. Called through Supexec, since the cookie
///				jar, the SuperVidel registers and the vector need supervisor
///				mode.
/// @return		0, or an ERR_ code
static long install(void)
{
	unsigned short sr;
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

	(void)Cconws("SV3TTY driver for TOS (step 2: link only, no BIOS devices yet)\r\n");

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
	default:
		(void)Cconws("sv3tty: cookie jar full. Not installed.\r\n");
		break;
	}
	if (r)
	{
		sv3t_wait_key(SV3T_START_WAIT);
		return 1;
	}

	sprintf(line, "sv3tty: SuperVidel firmware %u, %d ports, Timer C hooked, waiting for link\r\n",
			sv_firmware, NPORTS);
	(void)Cconws(line);

	// Time to read the messages when started from the desktop. The timer
	// is already hooked, so the link comes up meanwhile.
	sv3t_wait_key(SV3T_START_WAIT);

	(void)bufs;
	Ptermres(_PgmSize, 0);
	return 0;
}
