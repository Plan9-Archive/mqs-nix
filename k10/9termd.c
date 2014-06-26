#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#include "io.h"

Dev* devtab[] = {
	nil,
};

void
links(void)
{
}

Physseg physseg[8] = {
	{	.attr	= SG_SHARED,
		.name	= "shared",
		.size	= SEGMAXPG,
		.pgszi	= 1,
	},
	{	.attr	= SG_BSS,
		.name	= "memory",
		.size	= SEGMAXPG,
		.pgszi	= 1,
	},
};
int nphysseg = 8;

char dbgflg[256];

void (*mfcinit)(void) = nil;
void (*mfcopen)(Chan*) = nil;
int (*mfcread)(Chan*, uchar*, int, vlong) = nil;
void (*mfcupdate)(Chan*, uchar*, int, vlong) = nil;
void (*mfcwrite)(Chan*, uchar*, int, vlong) = nil;

void
rdb(void)
{
	splhi();
	iprint("rdb...not installed\n");
	for(;;)
		hardhalt();
}

char* conffile = "/sys/src/nix/k10/9termd";
ulong kerndate = KERNDATE;
