/*
 * $Id$
 *
 * This file belongs to FreeMiNT. It's not in the original MiNT 1.12
 * distribution. See the file CHANGES for a detailed log of changes.
 *
 *
 * Copyright 2003 Konrad M.Kokoszkiewicz <draco@atari.org>
 * All rights reserved.
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 * Author:  Konrad M.Kokoszkiewicz
 * Started: 15-01-2003
 *
 * please send suggestions, patches or bug reports to me or
 * the MiNT mailing list
 *
 */

# ifdef BUILTIN_SHELL

# include <stdarg.h>

# include "libkern/libkern.h"

# include "mint/basepage.h"	/* BASEPAGE struct */
# include "mint/mem.h"		/* F_FASTLOAD et contubernales */
# include "mint/stat.h"		/* struct stat */

# include "arch/cpu.h"		/* setstack() */
# include "arch/startup.h"	/* _base */
# include "arch/syscall.h"	/* Pexec(), Cconrs() et cetera */

# include "cnf.h"		/* init_env */
# include "dosmem.h"		/* m_free() */
# include "info.h"		/* national stuff */
# include "k_exec.h"		/* sys_pexec() */

# include "mis.h"

/* Note:
 *
 * 1) the shell is running as separate process, not in kernel context;
 * 2) the shell is running in user mode;
 * 3) the shell is very minimal, so don't expect much ;-)
 *
 */

/* XXX after the code stops changing so quickly, move all the messages to info.c
 */

# define SHELL_FLAGS	(F_FASTLOAD | F_ALTLOAD | F_ALTALLOC | F_PROT_P)

# define SH_VER_MAIOR	0
# define SH_VER_MINOR	1

# define COPYCOPY	"MiS v.%d.%d, the FreeMiNT internal shell,\r\n" \
			"(c) 2003 by Konrad M.Kokoszkiewicz (draco@atari.org)\r\n"

# define SHELL_STACK	32768L		/* maximum usage is so far about a half ot this */
# define SHELL_ARGS	2048L		/* number of pointers in the argument vector table (i.e. 8K) */

/* this is an average number of seconds in Gregorian year
 * (365 days, 6 hours, 11 minutes, 15 seconds).
 */
# define SEC_OF_YEAR	31558275L

# define LINELEN	126

# define SHCMD_EXIT	1
# define SHCMD_VER	2
# define SHCMD_LS	3
# define SHCMD_CD	4
# define SHCMD_CP	5
# define SHCMD_MV	6
# define SHCMD_RM	7
# define SHCMD_CHMOD	8
# define SHCMD_HELP	9
# define SHCMD_LN	10
# define SHCMD_EXEC	11
# define SHCMD_ENV	12
# define SHCMD_CHOWN	13
# define SHCMD_CHGRP	14
# define SHCMD_XCMD	15

/* Global variables */
static BASEPAGE *shell_base;
static short xcommands;

static const char *months[] =
{
	"Jan", "Feb", "Mar", "Apr", "May", "Jun", \
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* Utility routines */

static void
shell_print(char *text)
{
	Cconws(text);
}

static void
shell_printf(const char *fmt, ...)
{
	char buf[1024];
	va_list args;

	va_start(args, fmt);
	vsprintf(buf, sizeof (buf), fmt, args);
	va_end(args);

	shell_print(buf);
}

/* Helpers for ls:
 * justify_left() - just pads the text with spaces upto given lenght
 * justify_right() - shifts the text right adding spaces on the left
 *		until the given length is reached.
 */
static char *
justify_left(char *p, long spaces)
{
	long s = (spaces - strlen(p));

	while (*p && !isspace(*p))
		p++;

	while (s)
	{
		*p++ = ' ';
		s--;
	}

	return p;
}

static char *
justify_right(char *p, long spaces)
{
	long plen, s;
	char temp[32];

	plen = strlen(p);
	s = (spaces - plen);

	if (s > 0)
	{
		strcpy(temp, p);
		while (s)
		{
			*p++ = ' ';
			s--;
		}
		strcpy(p, temp);
	}

	return (p + plen);
}

/* Helper routines for manipulating environment */
static long
env_size(void)
{
	char *var;
	long count = 0;

	var = shell_base->p_env;

	if (var)
	{
		while (*var)
		{
			while (*var)
			{
				var++;
				count++;
			}
			var++;
			count++;
		}
	}

	return count;
}

static char *
env_append(char *where, char *what)
{
	strcpy(where, what);
	where += strlen(where);

	return ++where;
}

static char *
sh_getenv(const char *var)
{
	char *es, *env_str = shell_base->p_env;
	long vl, r;

	if (env_str == NULL || *env_str == 0)
	{
		shell_printf("mint: %s: no environment string!\r\n", __FUNCTION__);

		return 0;
	}

	vl = strlen(var);

	while (*env_str)
	{
		r = strncmp(env_str, var, vl);
		if (r == 0)
		{
			es = env_str;
			while (*es != '=')
				es++;
			es++;

			return es;
		}
		while (*env_str)
			env_str++;
		env_str++;
	}

	return 0;
}

/* `static void' when it gets used more often (once for now)
 */
static void
sh_setenv(const char *var, char *value)
{
	char *env_str = shell_base->p_env;
	char *new_env, *old_var, *es, *ne;
	long new_size, varlen;

	varlen = strlen(var);
	new_size = env_size();
	new_size += strlen(value);

	old_var = sh_getenv(var);
	if (old_var)
		new_size -= strlen(old_var);	/* this is the VALUE of the var */
	else
		new_size += varlen;		/* this is its NAME */

	new_size++;				/* trailing zero */

	new_env = (char *)Mxalloc(new_size, 3);
	if (new_env == NULL)
		return;

	bzero(new_env, new_size);

	es = env_str;
	ne = new_env;

	/* Copy old env to new place skipping `var' */
	while (*es)
	{
		if (strncmp(es, var, varlen) == 0)
		{
			while (*es)
				es++;
			es++;
		}
		else
		{
			while (*es)
				*ne++ = *es++;
			*ne++ = *es++;
		}
	}

	strcpy(ne, var);
	strcat(ne, value);

	ne += strlen(ne);
	ne++;
	*ne = 0;

	shell_base->p_env = new_env;

	Mfree(env_str);
}

/* Split command line into separate strings, put their pointers
 * into argv[], return argc.
 *
 * XXX add 'quoted arguments' handling.
 * XXX add wildcard expansion (at least the `*'), see fnmatch().
 *
 */
static long
crunch(char *cmdline, char *argv[])
{
	char *cmd = cmdline, *start;
	long cmdlen, idx = 0;

	cmdlen = strlen(cmdline);

	do
	{
		while (cmdlen && isspace(*cmd))			/* kill leading spaces */
		{
			cmd++;
			cmdlen--;
		}

		start = cmd;

		while (cmdlen && !isspace(*cmd))
		{
			cmd++;
			cmdlen--;
		}

		if (start == cmd)
			break;
		else if (*cmd)
		{
			*cmd = 0;
			cmd++;
			cmdlen--;
		}

		argv[idx] = start;
		idx++;

	} while (cmdlen > 0);

	argv[idx] = NULL;

	return idx;
}

/* Execute an external program */
static long
execvp(char *oldcmd, char *argv[])
{
	char *var, *new_env, *new_var, *t, *path, *np, npath[2048];
	long count = 0, x, r = -1L;

	var = shell_base->p_env;

	/* Check the size of the environment */
	count = env_size();
	count++;			/* trailing zero */
	count += strlen(oldcmd + 1);	/* add some space for the ARGV= strings */
	count += sizeof("ARGV=");	/* this is 6 bytes */

	new_env = (char *)Mxalloc(count, 3);

	if (!new_env)
		return -40;

	bzero(new_env, count + 1);

	var = shell_base->p_env;
	new_var = new_env;

	/* Copy variables from `var' to `new_var', but don't copy ARGV strings
	 */
	if (var)
	{
		while (*var)
		{
			if (strncmp(var, "ARGV=", 5) == 0)
				break;
			while (*var)
				*new_var++ = *var++;
			*new_var++ = *var++;
		}
	}

	/* Append new ARGV strings */
	x = 0;
	new_var = env_append(new_var, "ARGV=");

	while (argv[x])
	{
		new_var = env_append(new_var, argv[x]);
		x++;
	}
	*new_var = 0;

	*oldcmd = 0x7f;

	/* $PATH searching. Don't do it, if the user seems to
	 * have specified the explicit pathname.
	 */
	t = strrchr(argv[0], '/');
	if (!t)
	{
		path = sh_getenv("PATH=");

		if (path)
		{
			do
			{
				np = npath;

				while (*path && *path != ';' && *path != ',')
					*np++ = *path++;
				if (*path)
					path++;			/* skip the separator */

				*np = 0;
				strcat(npath, "/");
				strcat(npath, argv[0]);

				r = Pexec(0, npath, oldcmd, new_env);

			} while (*path && r < 0);
		}
	}
	else
		r = Pexec(0, argv[0], oldcmd, new_env);

	Mfree(new_env);

	return r;
}

static void
xcmdstate(void)
{
	shell_printf("Extended commands are %s\r\n", xcommands ? "on" : "off");
}

/* End utilities, begin commands */

static long
sh_ver(long argc, char **argv)
{
	shell_printf(COPYCOPY, SH_VER_MAIOR, SH_VER_MINOR);

	return 0;
}

static long
sh_help(long argc, char **argv)
{
	shell_printf( \
	"	MiS is not intended to be a regular system shell, so don't\r\n" \
	"	expect much. It is only a tool to fix bigger problems that\r\n" \
	"	prevent the system from booting normally.\r\n" \
	"\r\n" \
	"	Basic commands are:\r\n" \
	"\r\n" \
	"	cd - change directory\r\n" \
	"	exit - leave and reboot\r\n" \
	"	help - display this message\r\n" \
	"	ver - display version information\r\n" \
	"	xcmd - switch the extended command set on/off\r\n"
	"\r\n" \
	"	Extended commands (now %s) are:\r\n" \
	"\r\n" \
	"	chgrp - change the group a file belongs to\r\n" \
	"	chmod - change the access permissions for a file\r\n" \
	"	chown - change file's ownership\r\n" \
	"	*cp - copy file\r\n" \
	"	env - display environment\r\n" \
	"	*ln - create a link\r\n" \
	"	ls - display directory\r\n" \
	"	*mv - move/rename a file\r\n" \
	"	*rm - delete a file\r\n" \
	"\r\n" \
	"	All other words typed are understood as names of programs\r\n" \
	"	to execute. In case you'd want to execute something, that\r\n" \
	"	is named with one of the words displayed above, use 'exec'\r\n" \
	"	or supply the full pathname." \
	"\r\n", \
	xcommands ? "on" : "off");

	return 0;
}

/* Display all files in the current directory, with attributes et ceteris.
 * No wilcards filtering what to display, no sorted output, no nothing.
 */

static long
sh_ls(long argc, char **argv)
{
	struct stat st;
	struct timeval tv;
	short year, month, day, hour, minute;
	long r, s, handle;
	char *p, *dir, path[1024];
	char entry[256];

	dir = ".";

	if (argc >= 2)
	{
		if (strcmp(argv[1], "--help") == 0)
		{
			shell_printf("%s [dirspec]\r\n", argv[0]);

			return 0;
		}
		dir = argv[1];
	}

	r = Dopendir(dir, 0);
	if (r < 0)
		return r;

	tv.tv_sec = 0;
	Tgettimeofday(&tv, NULL);

	handle = r;

	do
	{
		r = Dreaddir(sizeof(entry), handle, entry);

		if (r == 0)
		{
			strcpy(path, dir);
			strcat(path, "/");
			strcat(path, entry + sizeof(long));

			/* `/bin/ls -l' doesn't dereference links, so we don't either */
			s = Fstat64(1, path, &st);

			/* XXX softlinks displayed should -> point to the file they point to */

			if (s == 0)
			{
				/* Reuse the path[] space */
				p = path;

				if (S_ISLNK(st.mode))		/* file type */
					*p++ = 'l';
				else if (S_ISMEM(st.mode))
					*p++ = 'm';
				else if (S_ISFIFO(st.mode))
					*p++ = 'p';
				else if (S_ISREG(st.mode))
					*p++ = '-';
				else if (S_ISBLK(st.mode))
					*p++ = 'b';
				else if (S_ISDIR(st.mode))
					*p++ = 'd';
				else if (S_ISCHR(st.mode))
					*p++ = 'c';
				else if (S_ISSOCK(st.mode))
					*p++ = 's';
				else
					*p++ = '?';

				/* access attibutes: user */
				*p++ = (st.mode & S_IRUSR) ? 'r' : '-';
				*p++ = (st.mode & S_IWUSR) ? 'w' : '-';
				if (st.mode & S_IXUSR)
					*p++ = (st.mode & S_ISUID) ? 's' : 'x';
				else
					*p++ = '-';

				/* ... group */
				*p++ = (st.mode & S_IRGRP) ? 'r' : '-';
				*p++ = (st.mode & S_IWGRP) ? 'w' : '-';
				if (st.mode & S_IXGRP)
					*p++ = (st.mode & S_ISGID) ? 's' : 'x';
				else
					*p++ = '-';

				/* ... others */
				*p++ = (st.mode & S_IROTH) ? 'r' : '-';
				*p++ = (st.mode & S_IWOTH) ? 'w' : '-';
				if (st.mode & S_IXOTH)
					*p++ = (st.mode & S_ISVTX) ? 't' : 'x';
				else
					*p++ = '-';

				*p++ = ' ';

				/* Unfortunately ksprintf() lacks many formatting features ...
				 */
				ksprintf(p, sizeof(path), "%d", (short)st.nlink);	/* XXX */
				p = justify_right(p, 5);
				*p++ = ' ';

				ksprintf(p, sizeof(path), "%ld", st.uid);
				p = justify_left(p, 9);

				ksprintf(p, sizeof(path), "%ld", st.gid);
				p = justify_left(p, 9);

				/* XXX this will cause problems, if st.size > 2GB */
				ksprintf(p, sizeof(path), "%ld", (long)st.size);
				p = justify_right(p, 8);
				*p++ = ' ';

				/* And now recalculate the time stamp */
				unix2calendar(st.mtime.time, &year, &month, &day, &hour, &minute, NULL);

				ksprintf(p, sizeof(path), "%s", months[month - 1]);
				p = justify_left(p, 4);

				ksprintf(p, sizeof(path), "%d", day);
				p = justify_left(p, 3);

				/* There can be up to 18 hours and 33 minutes of error
				 * relative to the calendar time, but for this purpose
				 * it does not matter.
				 */
				if ((tv.tv_sec - st.mtime.time) > (SEC_OF_YEAR >> 1))
					ksprintf(p, sizeof(path), "%d", year);
				else
					ksprintf(p, sizeof(path), (minute > 9) ? "%d:%d" : "%d:0%d", hour, minute);

				p = justify_right(p, 5);

				*p = 0;

				shell_printf("%s %s\r\n", path, entry + sizeof(long));
			}
		}
	} while (r == 0);

	Dclosedir(handle);

	return 0;
}

/* Change cwd to the given path. If none give, change to $HOME */
static long
sh_cd(long argc, char **argv)
{
	long r = 0;
	char *home;

	if (argc >= 2)
	{
		if (strcmp(argv[1], "--help") == 0)
			shell_printf("%s [newdir]\r\n", argv[0]);
		else
			r = Dsetpath(argv[1]);
	}
	else
	{
		home = sh_getenv("HOME=");
		if (home)
			r = Dsetpath(home);
	}

	return r;
}

/* chgrp, chown, chmod: change various attributes */
static long
sh_chgrp(long argc, char **argv)
{
	struct stat st;
	long r, gid;

	if (argc < 3)
	{
		shell_printf("%s DEC-GROUP file\r\n", argv[0]);

		return 0;
	}

	r = Fstat64(0, argv[2], &st);
	if (r < 0)
		return r;

	gid = atol(argv[1]);

	if (gid == st.gid)
		return 0;			/* no change */

	return Fchown(argv[2], st.uid, gid);
}

static long
sh_chown(long argc, char **argv)
{
	struct stat st;
	long r, uid, gid;
	char *own, *fname, *grp;

	if (argc < 3)
	{
		shell_printf("%s DEC-OWNER[:DEC-GROUP] file\r\n", argv[0]);

		return 0;
	}

	own = argv[1];
	fname = argv[2];

	r = Fstat64(0, fname, &st);
	if (r < 0)
		return r;

	uid = st.uid;
	gid = st.gid;

	/* cases like `chown 0 filename' and `chown 0. filename' */
	grp = strrchr(own, '.');
	if (!grp)
		grp = strrchr(own, ':');

	if (grp)
	{
		*grp++ = 0;
		if (isdigit(*grp))
			gid = atol(grp);
	}

	/* cases like `chown .0 filename' */
	if (isdigit(*own))
		uid = atol(own);

	if (uid == st.uid && gid == st.gid)
		return 0;			/* no change */

	return Fchown(fname, uid, gid);
}

static long
sh_chmod(long argc, char **argv)
{
	long d = 0;
	short c;
	char *s;

	if (argc < 3)
	{
		shell_printf("%s OCTAL-MODE file\r\n", argv[0]);

		return 0;
	}

	s = argv[1];

	while ((c = *s++) != 0 && isodigit(c))
	{
		d <<= 3;
		d += (c & 0x0007);
	}

	d &= S_IALLUGO;

	return Fchmod(argv[2], d);
}

static long
sh_env(long argc, char **argv)
{
	char *var;

	var = shell_base->p_env;

	if (var == NULL || *var == 0)
	{
		shell_printf("mint: %s: no environment string!\r\n", __FUNCTION__);

		return 0;
	}

	while (*var)
	{
		shell_printf("%s\r\n", var);
		while (*var)
			var++;
		var++;
	}

	return 0;
}

static long
sh_xcmd(long argc, char **argv)
{
	if (argc >= 2)
	{
		if (strcmp(argv[1], "on") == 0)
			xcommands = 1;
		else if (strcmp(argv[1], "off") == 0)
			xcommands = 0;
		else if (strcmp(argv[1], "--help") == 0)
			shell_printf("%s [on|off]\r\n", argv[0]);
	}
	xcmdstate();

	return 0;
}

static long
sh_exit(long argc, char **argv)
{
	short y;

	shell_print("Are you sure to exit and reboot (y/n)?");
	y = (short)Cconin();
	if (tolower(y & 0x00ff) == *MSG_init_menu_yes)
		Pterm(0);
	shell_print("\r\n");

	return 0;
}

static long
sh_cp(long argc, char **argv)
{
	return -1;
}

static long
sh_mv(long argc, char **argv)
{
	return -1;
}

static long
sh_rm(long argc, char **argv)
{
	return -1;
}

static long
sh_ln(long argc, char **argv)
{
	return -1;
}

static long
sh_exec(long argc, char **argv)
{
	return -1;
}

/* End of the commands, begin control routines */

static const char *commands[] =
{
	"exit", "ver", "ls", "cd", "cp", "mv", "rm", "chmod", \
	"help", "ln", "exec", "env", "chown", "chgrp", "xcmd", NULL
};

static const char is_ext[] =
{
	0, 0, 1, 0, 1, 1, 1, 1, \
	0, 1, 0, 0, 1, 1, 0	\
};

typedef long (FUNC)();

static FUNC *cmd_routs[] =
{
	sh_exit, sh_ver, sh_ls, sh_cd, sh_cp, sh_mv, sh_rm, sh_chmod, \
	sh_help, sh_ln, sh_exec, sh_env, sh_chown, sh_chgrp, sh_xcmd \
};

/* Execute the given command */
INLINE long
execute(char *cmdline)
{
	char *argv[SHELL_ARGS], newcmd[128], *c;
	long r, cnt, cmdno, argc;

	c = cmdline;

	/* Convert possible CR/LF characters to spaces */
	while (*c)
	{
		if (*c == '\r')
			*c = ' ';
		if (*c == '\n')
			*c = ' ';
		c++;
	}

	c = cmdline;

	/* Skip the first word (command) */
	while (*c && isspace(*c))
		c++;
	while (*c && !isspace(*c))
		c++;

	bzero(newcmd, sizeof(newcmd));

	strncpy(newcmd, c, 127);		/* crunch() destroys the `cmdline', so we need to copy it */

	argc = crunch(cmdline, argv);

	if (!argc)
		return 0;			/* empty command line (unlikely) */

	/* Result a zero if the string given is not an internal command,
	 * or the number of the internal command otherwise (the number is
	 * just their index in the commands[] table, plus one).
	 */
	cmdno = cnt = r = 0;

	while (commands[cnt])
	{
		if (strcmp(argv[0], commands[cnt]) == 0)
		{
			cmdno = cnt + 1;
			break;
		}
		cnt++;
	}

	/* If xcommands == 1 internal code is used for the commands
	 * below, or external programs are executed otherwise
	 */
	if (cmdno && !xcommands)
	{
		if (is_ext[cmdno - 1])
			cmdno = 0;
	}

	return (cmdno == 0) ? execvp(newcmd, argv) : cmd_routs[cmdno - 1](argc, argv);
}

/* XXX because of Cconrs() the command line cannot be longer than 126 bytes.
 */
static void
shell(void)
{
	char cwd[1024], linebuf[LINELEN+2], *lbuff, *home;
	long r;
	short x;

	/* XXX enable word wrap for the console, cursor etc. */
	shell_print("\r\n");
	sh_ver(0, NULL);
	xcmdstate();
	shell_print("Type `help' for help\r\n\r\n");

	home = sh_getenv("HOME=");
	if (home)
	{
		r = Dsetpath(home);
		if (r < 0)
			shell_printf("mint: %s: can't set home directory, error %ld\r\n\r\n", __FUNCTION__, r);
	}

	for (;;)
	{
		bzero(linebuf, sizeof(linebuf));

		linebuf[0] = (sizeof(linebuf) - 2);
		linebuf[1] = 0;

		r = Dgetcwd(cwd, 0, sizeof(cwd));

		if (*cwd == 0)
			strcpy(cwd, "/");
		else
		{
			for (x = 0;x < sizeof(cwd);x++)
			{
				if (cwd[x] == '\\')
					cwd[x] = '/';
			}
		}

		sh_setenv("PWD=", cwd);

		shell_printf("mint:%s#", cwd);
		Cconrs(linebuf);

		if (linebuf[1] > 1)
		{
			short idx;

			/* "the string is not guaranteed to be NULL terminated"
			 * (Atari GEMDOS reference manual)
			 */
			lbuff = linebuf + 2;
			idx = linebuf[1];
			idx--;
			lbuff[idx] = 0;

			r = execute(lbuff);

			if (r < 0)
				shell_printf("mint: %s: error %ld\r\n", lbuff, r);
		}
	}
}

/* Notice that we cannot define local variables here due to setstack()
 * which changes the stack pointer value.
 */
static void
shell_start(long bp)
{
	/* we must leave some bytes of `pad' behind the (sp)
	 * (see arch/syscall.S), this is why it is `-256L'.
	 */
	setstack(bp + SHELL_STACK - 256L);

	Mshrink((void *)bp, SHELL_STACK);

	(void)Pdomain(1);
	(void)Pumask(0x022);

	shell();			/* this one should never return */
}

/* This below is running in kernel context, called from init.c
 */
long
startup_shell(void)
{
	long r;

	shell_base = (BASEPAGE *)sys_pexec(7, (char *)SHELL_FLAGS, "\0", init_env);

	/* Hope this never happens */
	if (!shell_base)
	{
		DEBUG(("Pexec(7) returned 0!\r\n"));

		return -1;
	}

	/* Start address */
	shell_base->p_tbase = (long)shell_start;

	r = sys_pexec(104, (char *)"shell", shell_base, 0L);

	m_free((long)shell_base);

	return r;
}

# endif /* BUILTIN_SHELL */

/* EOF */
