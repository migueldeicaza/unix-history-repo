/*	@(#)shutdown.c	4.1 (Berkeley/Melbourne) 81/02/07	*/

#include <stdio.h>
#include <ctype.h>
#include <signal.h>
#include <utmp.h>
#include <time.h>
#include <sys/types.h>
/*
 *	/etc/shutdown when [messages]
 *
 *	allow super users to tell users and remind users
 *	of iminent shutdown of unix
 *	and shut it down automatically
 *	and even reboot or halt the machine if they desire
 *
 *		Ian Johnstone, Sydney, 1977
 *		Robert Elz, Melbourne, 1978
 *		Peter Lamb, Melbourne, 1980
 *		William Joy, Berkeley, 1981
 */
#define	REBOOT	"/etc/reboot"
#define	HALT	"/etc/halt"
#define MAXINTS 20
#define	HOURS	*3600
#define MINUTES	*60
#define SECONDS
#define NLOG		20		/* no of lines possible for message */
#define	NOLOGTIME	5 MINUTES

int	do_nothing();
time_t	getsdt();

extern	char *ctime();
extern	struct tm *localtime();

struct	utmp utmp;
int	sint;
int	stogo;
char	tpath[] =	"/dev/";
int	nlflag = 1;		/* nolog yet to be done */
int	killflg = 1;
int	reboot = 0;
int	halt = 0;
char	term[sizeof tpath + sizeof utmp.ut_line];
char	tbuf[BUFSIZ];
char	nolog1[] = "\n\nNO LOGINS: System going down at %5.5s\n\n";
char	*nolog2[NLOG+1];
#ifdef	DEBUG
char	nologin[] = "nologin";
#else
char	nologin[] = "/etc/nologin";
#endif
int slots;
struct interval {
	int stogo;
	int sint;
} interval[] = {
	4 HOURS,	1 HOURS,
	2 HOURS,	30 MINUTES,
	1 HOURS,	15 MINUTES,
	30 MINUTES,	10 MINUTES,
	15 MINUTES,	5 MINUTES,
	10 MINUTES,	2 MINUTES,
	2 MINUTES,	30 SECONDS,
	0 SECONDS,	0 SECONDS
};
main(argc,argv)
	int argc;
	char **argv;
{
	register i, ufd;
	register char **mess, *f;
	char *ts;
	long sdt;
	int h, m;
	long nowtime;
	FILE *termf;

	argc--, argv++;
	while (argc > 0 && (f = argv[0], *f++ == '-')) {
		while (i = *f++) switch (i) {
		case 'k':
			killflg = 0;
			continue;
		case 'r':
			reboot = 1;
			continue;
		case 'h':
			halt = 1;
			continue;
		default:
			fprintf(stderr, "shutdown: '%c' - unknown flag\n", i);
			exit(1);
		}
		argc--, argv++;
	}
	if (argc < 1) {
		printf("Usage: %s [-krd] shutdowntime [nologmessage]\n",
		    argv[0]);
		finish();
	}
	sdt = getsdt(argv[0]);
	argc--, argv++;
	i = 0;
	while (argc-- > 0)
		if (i < NLOG)
			nolog2[i++] = *argv++;
	nolog2[i] = NULL;
	nowtime = time((long *)0);
	m = ((stogo = sdt - nowtime) + 30)/60;
	h = m/60; 
	m %= 60;
	ts = ctime(&sdt);
	printf("Shutdown at %5.5s\n", ts+11);
	printf("ie. in ");
	if (h > 0)
		printf("%d hour%s ", h, h != 1 ? "s" : "");
	printf("%d minute%s\n", m, m != 1 ? "s" : "");
#ifndef DEBUG
	signal(SIGHUP, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	signal(SIGINT, SIG_IGN);
#endif
	signal(SIGTERM, finish);
	signal(SIGALRM, do_nothing);
	nice(-20);
	if (i = fork()) {
		printf("%d\n", i);
		exit(0);
	}
	sint = 1 HOURS;
	f = "";
	for (;;) {
		for (i = 0; stogo <= interval[i].stogo && interval[i].sint; i++)
			sint = interval[i].sint;
		if (stogo <= NOLOGTIME && nlflag) {
			nlflag = 0;
			nolog(sdt);
		}
		if (sint >= stogo || sint == 0)
			f = "FINAL ";
		ufd = open("/etc/utmp",0);
		nowtime = time((long *) 0);
		while (read(ufd,&utmp,sizeof utmp)==sizeof utmp)
		if (utmp.ut_name[0]) {
			strcpy(term, tpath);
			strncat(term, utmp.ut_line, sizeof utmp.ut_line);
			alarm(3);
#ifdef DEBUG
			if ((termf = fopen("/dev/tty", "w")) != NULL)
#else
			if ((termf = fopen(term, "w")) != NULL)
#endif
			{
				alarm(0);
				setbuf(termf, tbuf);
				fprintf(termf, "\n\n");
				warn(termf, sdt, nowtime);
				if (sdt - nowtime > 1 MINUTES)
					for (mess = nolog2; *mess; mess++)
						fprintf(termf, "%s ", *mess);
				fputc('\n', termf);
				fclose(termf);
				alarm(0);
			}
		}
		if (stogo < 0) {
	printf("\n\007\007System shutdown time has arrived\007\007\n");
			unlink(nologin);
			if (!killflg) {
				printf("but you'll have to do it yourself\n");
				finish();
			}
#ifndef DEBUG
			if (reboot)
				execle(REBOOT, "reboot", 0, 0);
			if (halt)
				execle(HALT, "halt", 0, 0);
			kill(1, SIGTERM);	/* sync */
			kill(1, SIGTERM);	/* sync */
			sleep(20);
#else
			printf("EXTERMINATE EXTERMINATE\n");
#endif
			finish();
		}
		stogo = sdt - time((long *) 0);
		if (stogo > 0)
			sleep(sint<stogo ? sint : stogo);
		stogo -= sint;
	}
}

time_t
getsdt(s)
register char *s;
{
	time_t t, t1, tim;
	register char c;
	struct tm *lt;

	if (*s == '+') {
		++s; 
		t = 0;
		for (;;) {
			c = *s++;
			if (!isdigit(c))
				break;
			t = t * 10 + c - '0';
		}
		if (t <= 0)
			t = 5;
		t *= 60;
		tim = time((long *) 0) + t;
		return(tim);
	}
	t = 0;
	while (strlen(s) > 2 && isdigit(*s))
		t = t * 10 + *s++ - '0';
	if (*s == ':')
		s++;
	if (t > 23)
		goto badform;
	tim = t*60;
	t = 0;
	while (isdigit(*s))
		t = t * 10 + *s++ - '0';
	if (t > 59)
		goto badform;
	tim += t; 
	tim *= 60;
	t1 = time((long *) 0);
	lt = localtime(&t1);
	t = lt->tm_sec + lt->tm_min*60 + lt->tm_hour*3600;
	if (tim < t || tim >= (24*3600)) {
		/* before now or after midnight */
		printf("That must be tomorrow\nCan't you wait till then?\n");
		finish();
	}
	return (t1 + tim -t);
badform:
	printf("Bad time format\n");
	finish();
}

warn(term, sdt, nowtime)
	FILE *term;
	long sdt, nowtime;
{
	char *ts;

	ts = ctime(&sdt);
	if (sdt - nowtime > 10 MINUTES)
		fprintf(term, "System going down at %5.5s\n", ts+11);
	else if ( sdt - nowtime > 60 SECONDS ) {
		fprintf(term, "System going down in %d minute%s\n",
		(sdt-nowtime+30)/60, (sdt-nowtime+30)/60 != 1 ? "s" : "");
	} else if ( sdt - nowtime > 0 ) {
		fprintf(term, "System going down in %d seconds\n",
		sdt-nowtime, sdt-nowtime != 1 ? "s" : "");
	} else
		fprintf(term, "System going down IMMEDIATELY\n");
}

nolog(sdt)
	long sdt;
{
	FILE *nologf;
	register char **mess;

	if ((nologf = fopen(nologin, "w")) != NULL) {
		fprintf(nologf, nolog1, (ctime(&sdt)) + 11);
		for (mess = nolog2; *mess; mess++)
			fprintf(nologf, "\t%s\n", *mess);
		fclose(nologf);
	}
}

finish()
{
	signal(SIGTERM, SIG_IGN);
	unlink(nologin);
	exit(0);
}

do_nothing()
{

	signal(SIGALRM, do_nothing);
}
