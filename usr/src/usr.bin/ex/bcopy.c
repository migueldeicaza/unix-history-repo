/* block copy from from to to, count bytes */
static char *sccsid = "@(#)bcopy.c	5.1 %G%";
bcopy(from, to, count)
#ifdef vax
	char *from, *to;
	int count;
{

	asm("	movc3	12(ap),*4(ap),*8(ap)");
}
#else
	register char *from, *to;
	register int count;
{
	while (count--)
		*to++ = *from++;
}
#endif
