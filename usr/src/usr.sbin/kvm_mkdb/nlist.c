/*-
 * Copyright (c) 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * %sccs.include.redist.c%
 */

#ifndef lint
static char sccsid[] = "@(#)nlist.c	8.1 (Berkeley) %G%";
#endif /* not lint */

#include <sys/param.h>

#include <a.out.h>
#include <db.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <kvm.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "extern.h"

typedef struct nlist NLIST;
#define	_strx	n_un.n_strx
#define	_name	n_un.n_name

#define	badfmt(str)	errx(1, "%s: %s: %s", kfile, str, strerror(EFTYPE))

static void badread __P((int, char *));

static char *kfile;

void
create_knlist(name, db)
	char *name;
	DB *db;
{
	register int nsyms;
	struct exec ebuf;
	FILE *fp;
	NLIST nbuf;
	DBT data, key;
	int fd, nr, strsize;
	char *strtab, buf[1024];

	kfile = name;
	if ((fd = open(name, O_RDONLY, 0)) < 0)
		err(1, "%s", name);

	/* Read in exec structure. */
	nr = read(fd, &ebuf, sizeof(struct exec));
	if (nr != sizeof(struct exec))
		badfmt("no exec header");

	/* Check magic number and symbol count. */
	if (N_BADMAG(ebuf))
		badfmt("bad magic number");
	if (!ebuf.a_syms)
		badfmt("stripped");

	/* Seek to string table. */
	if (lseek(fd, N_STROFF(ebuf), SEEK_SET) == -1)
		badfmt("corrupted string table");

	/* Read in the size of the symbol table. */
	nr = read(fd, (char *)&strsize, sizeof(strsize));
	if (nr != sizeof(strsize))
		badread(nr, "no symbol table");

	/* Read in the string table. */
	strsize -= sizeof(strsize);
	if (!(strtab = malloc(strsize)))
		err(1, NULL);
	if ((nr = read(fd, strtab, strsize)) != strsize)
		badread(nr, "corrupted symbol table");

	/* Seek to symbol table. */
	if (!(fp = fdopen(fd, "r")))
		err(1, "%s", name);
	if (fseek(fp, N_SYMOFF(ebuf), SEEK_SET) == -1)
		err(1, "%s", name);
	
	data.data = (u_char *)&nbuf;
	data.size = sizeof(NLIST);

	/* Read each symbol and enter it into the database. */
	nsyms = ebuf.a_syms / sizeof(struct nlist);
	while (nsyms--) {
		if (fread((char *)&nbuf, sizeof (NLIST), 1, fp) != 1) {
			if (feof(fp))
				badfmt("corrupted symbol table");
			err(1, "%s", name);
		}
		if (!nbuf._strx || nbuf.n_type&N_STAB)
			continue;

		key.data = (u_char *)strtab + nbuf._strx - sizeof(long);
		key.size = strlen((char *)key.data);
		if (db->put(db, &key, &data, 0))
			err(1, "record enter");

		if (strcmp((char *)key.data, VRS_SYM) == 0) {
			long cur_off, voff;
#ifndef KERNTEXTOFF
#define KERNTEXTOFF KERNBASE
#endif
			/*
			 * Calculate offset relative to a normal (non-kernel)
			 * a.out.  KERNTEXTOFF is where the kernel is really
			 * loaded; N_TXTADDR is where a normal file is loaded.
			 * From there, locate file offset in text or data.
			 */
			voff = nbuf.n_value - KERNTEXTOFF + N_TXTADDR(ebuf);
			if ((nbuf.n_type & N_TYPE) == N_TEXT)
				voff += N_TXTOFF(ebuf) - N_TXTADDR(ebuf);
			else
				voff += N_DATOFF(ebuf) - N_DATADDR(ebuf);
			cur_off = ftell(fp);
			if (fseek(fp, voff, SEEK_SET) == -1)
				badfmt("corrupted string table");

			/*
			 * Read version string up to, and including newline.
			 * This code assumes that a newline terminates the
			 * version line.
			 */
			if (fgets(buf, sizeof(buf), fp) == NULL)
				badfmt("corrupted string table");

			key.data = (u_char *)VRS_KEY;
			key.size = sizeof(VRS_KEY) - 1;
			data.data = (u_char *)buf;
			data.size = strlen(buf);
			if (db->put(db, &key, &data, 0))
				err(1, "record enter");

			/* Restore to original values. */
			data.data = (u_char *)&nbuf;
			data.size = sizeof(NLIST);
			if (fseek(fp, cur_off, SEEK_SET) == -1)
				badfmt("corrupted string table");
		}
	}
	(void)fclose(fp);
}

static void
badread(nr, p)
	int nr;
	char *p;
{
	if (nr < 0)
		err(1, "%s", kfile);
	badfmt(p);
}
