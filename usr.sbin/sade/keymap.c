/*
 * Copyright (c) 1996 Joerg Wunsch
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE DEVELOPERS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE DEVELOPERS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD$
 *
 */

#include "sade.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/kbio.h>

struct keymapInfo {
    const char *name;
    const struct keymap *map;
};

#include "keymap.h"

/*
 * keymap.h is being automatically generated by the Makefile.  It
 * contains definitions for all desired keymaps.  Note that since we
 * don't support font loading nor screen mapping during installation,
 * we simply don't care for any other keys than the ASCII subset.
 *
 * Therefore, if no keymap with the exact name has been found in the
 * first pass, we make a second pass over the table looking just for
 * the language name only.
 */

/*
 * Return values:
 *
 *  0: OK
 * -1: no appropriate keymap found
 * -2: error installing map (other than ENXIO which means we're not on syscons)
 */

int
loadKeymap(const char *lang)
{
    int passno, err;
    char *llang;
    size_t l;
    struct keymapInfo *kip;

    llang = strdup(lang);
    if (llang == NULL)
	abort();

    for (passno = 0; passno < 2; passno++)
    {
	if (passno > 0) 
	{
	    /* make the match more fuzzy */
	    l = strspn(llang, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
	    llang[l] = '\0';
	}

	l = strlen(llang);

	for (kip = keymapInfos; kip->name; kip++)
	    if (strncmp(kip->name, llang, l) == 0)
	    {
		/* Yep, got it! */
		err = ioctl(0, PIO_KEYMAP, kip->map);
		free(llang);
		return (err == -1 && errno != ENOTTY)? -2: 0;
	    }
    }
    free(llang);
    return -1;
}
