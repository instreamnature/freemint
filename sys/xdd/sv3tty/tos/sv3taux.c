/*
 * sv3taux.c
 *
 * Maps AUX: (BIOS device 1) to an sv3tty port with Bconmap, so programs
 * that only use AUX: (e.g. the "Serial" port of a terminal program) talk
 * to Linux through the sv3tty driver.
 *
 * Usage: SV3TAUX.TTP [port]     map AUX: to this port (0..15, default 0)
 *        SV3TAUX.TTP off        map AUX: back to BIOS device 6 (the MFP
 *                               serial port on a Falcon)
 */

#include <stdio.h>
#include <stdlib.h>
#include <osbind.h>

#include "sv3tty_tos.h"


static long cookie_value;


/// @brief		Look for our cookie, called through Supexec
/// @return		1 if found
static long find_driver(void)
{
	return sv3t_get_cookie(SV3T_COOKIE, &cookie_value);
}


int main(int argc, char *argv[])
{
	struct sv3t_info *info;
	long port = 0, dev, old;

	if (!Supexec(find_driver))
	{
		printf("sv3taux: the sv3tty driver is not installed\n");
		fflush(stdout);
		sv3t_wait_key(0);
		return 1;
	}
	info = (struct sv3t_info *)cookie_value;
	if (info->magic != SV3T_COOKIE || info->version != SV3T_INFO_VERSION)
	{
		printf("sv3taux: unknown driver version %u\n", info->version);
		fflush(stdout);
		sv3t_wait_key(0);
		return 1;
	}

	if (argc > 1 && (argv[1][0] == 'o' || argv[1][0] == 'O'))
		dev = 6;
	else
	{
		if (argc > 1)
			port = atol(argv[1]);
		if (port < 0 || port >= info->nports)
		{
			printf("sv3taux: port must be 0..%u\n", info->nports - 1);
			fflush(stdout);
			sv3t_wait_key(0);
			return 1;
		}
		dev = info->first_dev + port;
	}

	// Bconmap(dev) maps AUX: to dev and returns the device it was before
	old = Bconmap((short)dev);
	if (dev == 6)
		printf("sv3taux: AUX: is BIOS device 6 again (was %ld)\n", old);
	else
		printf("sv3taux: AUX: is now BIOS device %ld = SV3TTY%ld (was %ld)\n", dev, port, old);

	fflush(stdout);
	sv3t_wait_key(SV3T_START_WAIT);
	return 0;
}
