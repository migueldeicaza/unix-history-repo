/*
 * Copyright (c) 1992, 1993, 1996
 *	Berkeley Software Design, Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Berkeley Software
 *	Design, Inc.
 *
 * THIS SOFTWARE IS PROVIDED BY Berkeley Software Design, Inc. ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL Berkeley Software Design, Inc. BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	BSDI doscmd.h,v 2.3 1996/04/08 19:32:32 bostic Exp
 *
 * $FreeBSD$
 */


#ifdef __NetBSD__
#define USE_VM86
#endif

#include <sys/param.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/signalvar.h>
#include <machine/sigframe.h>

#include <machine/frame.h>
#include <machine/psl.h>
#include <machine/npx.h>
#ifdef USE_VM86
#include <machine/vm86.h>
#endif

#include "register.h"
#include "dos.h"
#include "callback.h"

#define drlton(a)	((islower((a)) ? toupper((a)) : (a)) - 'A')
#define drntol(a)	((a) + 'A')

/* 
** assorted hardware/scope constants 
*/

#define	MAX_AVAIL_SEG	0xa000

#define MAXPORT		0x400

#define N_PARALS_MAX	3
#define N_COMS_MAX	4	/* DOS restriction (sigh) */

struct vconnect_area {
        int     int_state;
        int     magic;                  /* 0x4242 -> PRB format */
        u_long  passthru[256>>5];       /* bitmap of INTs to handle */
        u_long  magiciret[2];           /* Bounds of "magic" IRET */
};
extern struct vconnect_area vconnect_area;
#define IntState vconnect_area.int_state

/* ParseBuffer.c */
extern int	ParseBuffer(char *, char **, int);

/* bios.c */
#define	BIOSDATA	((u_char *)0x400)
extern unsigned long	rom_config;
extern int nfloppies;
extern int ndisks;
extern int nserial;
extern int nparallel;

extern volatile int	poll_cnt;
extern void		bios_init(void);
extern void		wakeup_poll(void);
extern void		reset_poll(void);
extern void		sleep_poll(void);

/* cmos.c */
extern time_t	delta_clock;

extern void	cmos_init(void);

/* config.c */
extern int	read_config(FILE *fp);

/* cpu.c */
extern void	cpu_init(void);
extern int	emu_instr(regcontext_t *);

/* debug.c */
extern int	vflag;
extern int	tmode;
extern FILE	*debugf;
extern int	debug_flags;

/* Lower 8 bits are int number */
#define D_ALWAYS 	0x0000100	/* always emit this message */
#define D_TRAPS 	0x0000200	/* trap-related activity */
#define D_FILE_OPS	0x0000400	/* file-related activity */
#define D_MEMORY	0x0000800	/* memory-related activity */
#define D_HALF		0x0001000 	/* "half-implemented" system calls */
#define	D_FLOAT		0x0002000	/* ??? */
#define	D_DISK		0x0004000	/* disk (not file) operations */
#define	D_TRAPS2	0x0008000
#define	D_PORT		0x0010000	/* port accesses */
#define	D_EXEC		0x0020000
#define	D_ITRAPS	0x0040000
#define	D_REDIR		0x0080000	/* redirector functions */
#define	D_PRINTER	0x0100000
#define	D_TRAPS3	0x0200000
#define	D_DEBUGIN	0x0400000
#define D_DOSCALL	0x0800000	/* MS-DOS function results */
#define D_XMS		0x1000000	/* XMS calls */
#define D_EMS		0x2000000	/* EMS calls */
#define D_VIDEO		0x4000000	/* video-related activity */

#define	TTYF_ECHO	0x00000001
#define	TTYF_ECHONL	0x00000003
#define	TTYF_CTRL	0x00000004
#define	TTYF_BLOCK	0x00000008
#define	TTYF_POLL	0x00000010
#define	TTYF_REDIRECT	0x00010000	/* Cannot have 0xffff bits set */

#define	TTYF_ALL	(TTYF_ECHO | TTYF_CTRL | TTYF_REDIRECT)
#define	TTYF_BLOCKALL	(TTYF_ECHO | TTYF_CTRL | TTYF_REDIRECT | TTYF_BLOCK)

extern void	unknown_int2(int, int, regcontext_t *REGS);
extern void	unknown_int3(int, int, int, regcontext_t *REGS);
extern void	unknown_int4(int, int, int, int, regcontext_t *REGS);
extern void	fatal(char *fmt, ...) __printflike(1, 2);
extern void	debug(int flags, char *fmt, ...) __printflike(2, 3);
extern void	dump_regs(regcontext_t *REGS);
extern void	debug_set(int x);
extern void	debug_unset(int x);
extern u_long	debug_isset(int x);

/* disktab.c */
extern int	map_type(int, int *, int *, int *);

/* doscmd.c */
extern int	squirrel_fd(int);

extern int		capture_fd;
extern int		dead;
extern int		xmode;
extern int		booting;
extern int		raw_kbd;
extern int		timer_disable;
extern char		cmdname[];
extern struct timeval	boot_time;
extern unsigned long	*ivec;

extern int		open_prog(char *name);
extern void		done(regcontext_t *REGS, int val);
extern void 		quit(int);
extern void		call_on_quit(void (*)(void *), void *);
extern void		iomap_port(int port, int count);

/* ems.c */
extern int ems_init();
extern void ems_entry(regcontext_t *REGS);
extern u_long ems_frame_addr;

/* emuint.c */
extern void	emuint(regcontext_t *REGS);

/* i386-pinsn.c */
extern int	i386dis(unsigned short, unsigned short,
    unsigned char *, char *, int);

/* int.c */
extern void	softint(int intnum);
extern void	hardint(int intnum);

extern void	delay_interrupt(int intnum, void (*func)(int));
extern void	resume_interrupt(void);

/* int10.c */
extern void	int10(regcontext_t *);

/* int13.c */
extern int	init_hdisk(int drive, int cyl, int head, int tracksize,
			   char *file, char *boot_sector);
extern int	init_floppy(int drive, int type, char *file);
extern int	disk_fd(int drive);
extern void	make_readonly(int drive);
extern int	search_floppy(int i);
extern void	disk_bios_init(void);

/* int17.c */
extern void	lpt_poll(void);
extern void	printer_direct(int printer);
extern void	printer_spool(int printer, char *print_queue);
extern void	printer_timeout(int printer, char *time_out);

/* int2f.c */
extern void	int2f(regcontext_t *);

/* intff.c */
extern int	int2f_11(regcontext_t *REGS);
extern void	intff(regcontext_t *REGS);

/* mem.c */
extern char	*dosmem;

extern void	mem_init(void);
extern int	mem_alloc(int size, int owner, int *biggestp);
extern int	mem_adjust(int addr, int size, int *availp);
extern void	mem_free_owner(int owner);
extern void	mem_change_owner(int addr, int owner);

/* mouse.c */
extern void	mouse_init(void);

/* net.c */
extern void	net_init(void);

/* port.c */
extern void	define_input_port_handler(int, unsigned char (*)(int));
extern void	define_output_port_handler(int, void (*)(int, unsigned char));
extern void	inb(regcontext_t *, int);
extern void	init_io_port_handlers(void);
extern void	inx(regcontext_t *, int);
extern void	outb(regcontext_t *, int);
extern void	outx(regcontext_t *, int);
extern void	speaker_init(void);
extern void	outb_traceport(int, unsigned char);
extern unsigned char	inb_traceport(int);
extern void	outb_port(int, unsigned char);
extern unsigned char	inb_port(int);

/* setver.c */
extern void	setver(char *, short);
extern short	getver(char *);

/* signal.c */
extern struct sigframe	*saved_sigframe;
extern regcontext_t	*saved_regcontext;
extern int		saved_valid;
extern void		setsignal(int s, void (*h)(struct sigframe *));

/* timer.c */
extern void	timer_init(void);

/* trace.c */
extern int	resettrace(regcontext_t *);
extern void	tracetrap(regcontext_t *);

/* xms.c */
extern void	get_raw_extmemory_info(regcontext_t *REGS);
extern int	int2f_43(regcontext_t *REGS);
extern void	initHMA(void);
extern void	xms_init(void);
extern u_long	xms_maxsize;

/****************************** dirty below here *****************************/

extern u_long	pending[];		/* pending interrupts */
extern int	n_pending;

extern int nmice;

void put_dosenv(char *value);
