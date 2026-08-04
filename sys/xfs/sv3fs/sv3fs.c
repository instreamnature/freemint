/*
 * The Host OS filesystem access driver - entry point.
 *
 * This file belongs to FreeMiNT. It's not in the original MiNT 1.12
 * distribution. See the file CHANGES for a detailed log of changes.
 *
 * Copyright (c) 2002-2006 Standa of ARAnyM dev team.
 * Copyright 1998, 1999, 2001 by Markus Kohm <Markus.Kohm@gmx.de>.
 * Modified by Chris Felsch <C.Felsch@gmx.de>.
 *
 * Originally taken from the STonX CVS repository.
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
 */

#include "global.h"
#include "sv3fs_xfs.h"
#include "sv3fs_dev.h"
#include "sv3fs.h"

#include "mint/kerinfo.h"
#include "mint/arch/nf_ops.h"


/*
 * filesystem basic description
 */
static struct fs_descr sv3fs_descr =
    {
	NULL,
	0, /* this is filled in by MiNT at FS_MOUNT */
	0, /* FIXME: what about flags? */
	{0,0,0,0}  /* reserved */
    };

#if (0)
#ifdef __KERNEL__
	#if __KERNEL__ == 0
		#error ("__KERNEL__ is 0")
	#elif __KERNEL__ == 1
		#error ("__KERNEL__ is 1")
	#elif __KERNEL__ == 2
		#error ("__KERNEL__ is 2")
	#elif __KERNEL__ == 3
		#error ("__KERNEL__ is 3")
	#else
		#error ("__KERNEL__ is not 0,1,2,3")
	#endif
#else
	#error ("__KERNEL__ is undefined!")
#endif
#endif


#if __KERNEL__ == 1

FILESYS *sv3fs_mount_drives(FILESYS *fs)
{
	long r;
	ulong drv_mask = fs_drive_bits();
	ushort drv_number = 0;
	char mount_point[] = "u:\\XX";

	c_conws("mounts: ");

	while ( drv_mask ) {
		/* search the 1st log 1 bit position -> drv_number */
		while( ! (drv_mask & 1) ) { drv_number++; drv_mask>>=1; }

		mount_point[3] = drv_number+'a';
		mount_point[4] = '\0';

		DEBUG(("sv3fs: drive: %d", drv_number));

		c_conws( (const char*)mount_point );
		c_conws(" ");

		/* r = d_cntl(FS_MOUNT, mount_point, (long) &sv3fs_descr); */
		/* the d_cntl(FS_MOUNT) starts with dev_no = 100 */
		sv3fs_descr.dev_no = 50+drv_number;
		sv3fs_descr.file_system = fs;
		DEBUG(("sv3fs: Dcnt(FS_MOUNT) dev_no: %d", sv3fs_descr.dev_no));

		r = fs_native_init( sv3fs_descr.dev_no, mount_point, "/", 0 /*caseSensitive*/,
				fs, &sv3fs_fs_devdrv );
		/* set the drive bit */
		if ( !r )
			*((long *) 0x4c2L) |= 1UL << drv_number;

		drv_number++; drv_mask>>=1;
	}
	c_conws("\r\n");
	c_conws("\r\n");

	return fs;
}

#else /* __KERNEL__ == 1 */

FILESYS *sv3fs_mount_drives(FILESYS *fs)
{
	long r;
	int succ = 0;
	int keep = 0;
	int rollback_failed = 0;

	/**
	 * FIXME: TODO: If there are really different settings in the filesystem
	 * mounting to provide full case sensitive filesystem (like ext2) then
	 * we need to strip the FS_NOXBIT from the filesystem flags. For now
	 * every mount is half case sensitive and behaves just like mint's fatfs
	 * implementation in this sense.
	 *
	 * We would also need to allocate a duplicate copy if there are different
	 * styles of case sensitivity used in order to provide the kernel with
	 * appropriate filesystem information. Also some NF function would be needed
	 * to go though the configuration from the sv3 side (at this point there
	 * is only the fs_drive_bits() which doesn't say much).
	 **/

	sv3fs_descr.file_system = fs;

	/* Install the filesystem */
	r = d_cntl (FS_INSTALL, "u:\\", (long) &sv3fs_descr);
	DEBUG(("sv3fs: Dcntl(FS_INSTALL) descr=%x", &sv3fs_descr));
	if (r != 0 && r != (long)KERNEL)
	{
		DEBUG(("Return value was %li", r));

		/* Nothing installed, so nothing to stay resident */
		return NULL;
	}

	{
		ulong drv_mask = fs_drive_bits();
		ushort drv_number = 0;
		char mount_point[] = "u:\\XX";

		succ |= 1;

		c_conws("\r\nMounts: ");

		while ( drv_mask ) {
			/* search the 1st log 1 bit position -> drv_number */
			while( ! (drv_mask & 1) ) { drv_number++; drv_mask>>=1; }

			/* ready */
			succ = 0;

			mount_point[3] = drv_number+'a';
			mount_point[4] = '\0';

			DEBUG(("sv3fs: drive: %d", drv_number));

			c_conws( (const char*)mount_point );
			c_conws(" ");

			/* mount */
			r = d_cntl(FS_MOUNT, mount_point, (long) &sv3fs_descr);
			DEBUG(("sv3fs: Dcnt(FS_MOUNT) dev_no: %d", sv3fs_descr.dev_no));
			if (r != sv3fs_descr.dev_no )
			{
				DEBUG(("sv3fs: return value was %li", r));
			} else {
				succ |= 2;
				/* init */

				r = fs_native_init( sv3fs_descr.dev_no, mount_point, "/", 0 /*caseSensitive*/,
										   fs, &sv3fs_fs_devdrv );
				DEBUG(("sv3fs: native_init mount_point: %s", mount_point));
				if ( r < 0 ) {
					DEBUG(("sv3fs: return value was %li", r));
				} else {
					succ = 0; /* do not unmount */
					keep = 1; /* at least one is mounted */
				}
			}

			/* Try to uninstall, if necessary */
			if ( succ & 2 ) {
				/* unmount */
				r = d_cntl(FS_UNMOUNT, mount_point,
						   (long) &sv3fs_descr);
				DEBUG(("sv3fs: Dcntl(FS_UNMOUNT) descr=%x", &sv3fs_descr));
				if ( r < 0 ) {
					DEBUG(("sv3fs: return value was %li", r));
					/* Can't uninstall, because unmount failed */
					rollback_failed |= 2;
				}
			}

			drv_number++; drv_mask>>=1;
		}

		c_conws("\r\n");
	}

	/* everything OK */
	if ( keep )
		return fs; /* We where successfull */

	/* Something went wrong here -> uninstall the filesystem */
	r = d_cntl(FS_UNINSTALL, "u:\\", (long) &sv3fs_descr);
	DEBUG(("sv3fs: Dcntl(FS_UNINSTALL) descr=%x", &sv3fs_descr));
	if ( r < 0 ) {
		DEBUG(("sv3fs: return value was %li", r));
		/* Can't say NULL,
		 * because uninstall failed */
		rollback_failed |= 1;
	}

       	/* Can't say NULL, if IF_UNINSTALL or FS_UNMOUNT failed */
	if ( rollback_failed ) return (FILESYS *) 1;

       	/* Nothing installed, so nothing to stay resident */
	return NULL;
}

#endif /* __KERNEL__ == 1 */
