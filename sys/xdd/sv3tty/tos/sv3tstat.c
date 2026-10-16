/*
 * sv3tstat.c
 *
 * Shows the state of the sv3tty TOS driver: link, counters and open ports.
 * Finds the driver through its 'SV3T' cookie.
 *
 * Usage: SV3TSTAT.TOS
 */

#include <stdio.h>
#include <osbind.h>

#include "sv3tty_tos.h"


static long cookie_value;

#define MEASURE_SECONDS		5


/// @brief		Read _hz_200 (TOS' 200 Hz counter), called through Supexec
/// @return		Counter value
static long read_hz200(void)
{
	return *(volatile long *)0x4baL;
}


/// @brief		Measure how often the driver's timer function runs. Its tick
///				counter is compared with TOS' _hz_200 over a few seconds.
///				Both are counted in the same Timer C interrupt, so they must
///				grow by the same amount: one driver call per Timer C tick.
/// @param info	Driver info from the cookie
static void measure_ticks(struct sv3t_info *info)
{
	unsigned long t0, t1, h0, h1, dt, dh, rate10;

	printf("  measuring the tick rate for %d seconds...\n", MEASURE_SECONDS);
	fflush(stdout);

	// Start right after a _hz_200 change, so both counters are read close
	// to the same tick
	h0 = Supexec(read_hz200);
	while ((unsigned long)Supexec(read_hz200) == h0)
		;
	h0 = Supexec(read_hz200);
	t0 = info->ticks;

	while ((unsigned long)Supexec(read_hz200) - h0 < 200UL * MEASURE_SECONDS)
		;

	h1 = Supexec(read_hz200);
	t1 = info->ticks;

	dt = t1 - t0;
	dh = h1 - h0;
	rate10 = dh ? (dt * 2000UL + dh / 2) / dh : 0;	// Rate in 0.1 Hz

	printf("  driver ticks %lu, _hz_200 ticks %lu: %lu.%lu Hz\n",
		   dt, dh, rate10 / 10, rate10 % 10);
	if (dt + 1 < dh || dt > dh + 1)
		printf("  NOTE: not one driver call per Timer C tick\n");
}


/// @brief		Look for our cookie, called through Supexec
/// @return		1 if found
static long find_driver(void)
{
	return sv3t_get_cookie(SV3T_COOKIE, &cookie_value);
}


int main(void)
{
	struct sv3t_info *info;
	struct sv3p *s;
	unsigned short i;

	if (!Supexec(find_driver))
	{
		printf("sv3tstat: the sv3tty driver is not installed\n");
		fflush(stdout);
		sv3t_wait_key(0);
		return 1;
	}

	info = (struct sv3t_info *)cookie_value;
	if (info->magic != SV3T_COOKIE || info->version != SV3T_INFO_VERSION)
	{
		printf("sv3tstat: unknown driver version %u\n", info->version);
		fflush(stdout);
		sv3t_wait_key(0);
		return 1;
	}
	s = info->proto;

	printf("sv3tty driver: %u ports, SuperVidel firmware %u, budget %u/tick\n",
		   info->nports, info->sv_firmware, info->tick_budget);
	printf("  BIOS devices %u..%u (Bconmap had %u rows before)\n",
		   info->first_dev, info->first_dev + info->nports - 1, info->old_maptab);
	printf("  link %s (peer v%u), ticks %lu, resyncs %lu, skipped_words %lu\n",
		   s->link_up ? "UP" : "WAIT", s->peer_version, info->ticks,
		   s->resyncs, s->skipped_words);
	printf("  rx: msgs %lu bytes %lu dropped %lu\n",
		   s->rx_msgs, s->rx_bytes, s->rx_dropped);
	printf("  rx err: bad_hdr %lu timeouts %lu unknown %lu\n",
		   s->rx_bad_hdr, s->rx_timeouts, s->rx_unknown);
	printf("  tx: msgs %lu bytes %lu timeouts %lu\n",
		   s->tx_msgs, s->tx_bytes, s->tx_timeouts);

	for (i = 0; i < s->nports; i++)
	{
		struct sv3p_port *p = &s->ports[i];

		if (!p->open && !p->announced)
			continue;
		printf("  port %u: open %u announced %u credited %u carrier %u tx_credit %lu rx_total %lu\n",
			   i, p->open, p->announced, p->credited, p->carrier, p->tx_credit, p->rx_total);
	}

	measure_ticks(info);

	// Wait, so the text can be read when started from the desktop
	fflush(stdout);
	sv3t_wait_key(0);
	return 0;
}
