/*
char id_format[] = "@(#)format.h	1.4";
 *
 * format parser definitions
 */

struct syl
{
	short op,p1,p2,rpcnt;
};

#define RET	1
#define REVERT 	2
#define GOTO 	3
#define X 	4
#define SLASH 	5
#define STACK 	6
#define I 	7
#define ED 	8
#define NED 	9
#define IM 	10
#define APOS 	11
#define H 	12
#define TL 	13
#define TR 	14
#define T 	15
#define COLON 	16
#define S 	17
#define SP 	18
#define SS 	19
#define P 	20
#define BNZ 	21
#define B 	22
#define F 	23
#define E 	24
#define EE 	25
#define D 	26
#define DE	27		/*** NOT STANDARD FORTRAN ***/
#define G 	28
#define GE 	29
#define L 	30
#define A 	31
#define AW	32
#define R	33		/*** NOT STANDARD FORTRAN ***/
#define DOLAR	34		/*** NOT STANDARD FORTRAN ***/
#define SU	35		/*** NOT STANDARD FORTRAN ***/

#define	FMTUNKN	-1
#define FMTOK	1
#define FMTERR	0

#define FMT_COMP 0x101		/* indicates pre-compiled formats */

extern struct syl *syl_ptr;
extern int parenlvl,revloc;
extern short pc;
