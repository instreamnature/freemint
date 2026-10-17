/*
 * sv3tclos.c
 *
 * Closes (hangs up) sv3tty ports of the TOS driver: CLOSE is sent to
 * Linux, which ends the session on the tty. The next access to the port
 * opens it again, and Linux shows a fresh login prompt.
 *
 * The BIOS has no close, so a terminal program cannot hang up a port
 * itself; run this after the session instead.
 *
 * Usage: SV3TCLOS.TTP [port ...]     close these ports (0..15)
 *        SV3TCLOS.TTP all            close all open ports
 *        SV3TCLOS.TTP                close port 0
 */

#include <stdio.h>
#include <stdlib.h>
#include <osbind.h>

#include "sv3tty_tos.h"


static long cookie_value;
static long port_to_close;


/// @brief		Look for our cookie, called through Supexec
/// @return		1 if found
static long find_driver(void)
{
	return sv3t_get_cookie(SV3T_COOKIE, &cookie_value);
}


/// @brief		Call the driver's close_port in supervisor mode (Supexec)
/// @return		0, or -1 for a bad port
static long do_close(void)
{
	struct sv3t_info *info = (struct sv3t_info *)cookie_value;

	return info->close_port(port_to_close);
}


/// @brief		Close one port and report it
/// @param info	Driver info
/// @param port	Port number
static void close_one(struct sv3t_info *info, long port)
{
	port_to_close = port;
	if (Supexec(do_close))
		printf("sv3tclos: no port %ld (0..%u)\n", port, info->nports - 1);
	else
		printf("sv3tclos: port %ld (BIOS device %ld, SV3TTY%ld) closed\n",
			   port, info->first_dev + port, port);
}


int main(int argc, char *argv[])
{
	struct sv3t_info *info;
	int i;

	if (!Supexec(find_driver))
	{
		printf("sv3tclos: the sv3tty driver is not installed\n");
		fflush(stdout);
		sv3t_wait_key(0);
		return 1;
	}
	info = (struct sv3t_info *)cookie_value;
	if (info->magic != SV3T_COOKIE || info->version != SV3T_INFO_VERSION)
	{
		printf("sv3tclos: unknown driver version %u\n", info->version);
		fflush(stdout);
		sv3t_wait_key(0);
		return 1;
	}

	if (argc < 2)
		close_one(info, 0);
	else if (argv[1][0] == 'a' || argv[1][0] == 'A')
	{
		// All ports that are open
		for (i = 0; i < info->nports; i++)
		{
			if (info->proto->ports[i].open)
				close_one(info, i);
		}
	}
	else
	{
		for (i = 1; i < argc; i++)
			close_one(info, atol(argv[i]));
	}

	fflush(stdout);
	sv3t_wait_key(SV3T_START_WAIT);
	return 0;
}
