/*
 * cookie.c
 *
 * Cookie jar lookup for the sv3tty TOS driver and its tools.
 */

#include <osbind.h>

#include "sv3tty_tos.h"

struct cookie
{
	long	id;
	long	value;
};

/* _p_cookies system variable: pointer to the cookie jar */
#define P_COOKIES		(*(struct cookie **)0x5a0L)


/// @brief		Read _hz_200, called through Supexec
/// @return		200 Hz tick counter
static long read_hz200(void)
{
	return *(volatile long *)0x4baL;
}


/// @brief			Wait for a key, at most a number of seconds (0 = no limit),
///					so the text can be read before a program started from the
///					desktop ends. User mode.
/// @param seconds	Max wait, 0 = wait for a key
void sv3t_wait_key(int seconds)
{
	long end = Supexec(read_hz200) + 200L * seconds;

	(void)Cconws(seconds ? "Press a key (continues in a few seconds)\r\n"
						 : "Press a key\r\n");
	while (!Bconstat(2))
	{
		if (seconds && Supexec(read_hz200) - end >= 0)
			return;
	}
	(void)Bconin(2);
}


/// @brief			Look for a cookie. Supervisor mode only.
/// @param id		Cookie id, e.g. 'SupV'
/// @param value	Gets the cookie value, may be 0
/// @return			1 if found, 0 otherwise
int sv3t_get_cookie(long id, long *value)
{
	struct cookie *c = P_COOKIES;

	if (!c)
		return 0;
	for (; c->id; c++)
	{
		if (c->id == id)
		{
			if (value)
				*value = c->value;
			return 1;
		}
	}
	return 0;
}
