#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "apic.h"
#include "sipi.h"

#define SIPIHANDLER	(KZERO+0x3000)

void
sipi(void)
{
	Apic *apic;
	Mach *mach;
	int apicno, i, nproc;
	u32int *sipiptr;
	uintmem sipipa;
	u8int *alloc, *p;
	extern void squidboy(int);

	/*
	 * Move the startup code into place,
	 * must be aligned properly.
	 */
	sipipa = mmuphysaddr(SIPIHANDLER);
	if((sipipa & (4*KiB - 1)) || sipipa > (1*MiB - 2*4*KiB))
		panic("sipi: invalid sipipa");
	sipiptr = UINT2PTR(SIPIHANDLER);
	memmove(sipiptr, sipihandler, sizeof(sipihandler));
	DBG("sipiptr %#p sipipa %#P\n", sipiptr, sipipa);

	/*
	 * Notes:
	 * The Universal Startup Algorithm described in the MP Spec. 1.4.
	 * The data needed per-processor is the sum of the stack, page
	 * table pages, vsvm page and the Mach page. The layout is similar
	 * to that described in dat.h for the bootstrap processor, but
	 * with any unused space elided.
	 */
	nproc = 0;
	for(apicno = 0; apicno < Napic; apicno++){
		if((apic = lapiclookup(apicno)) == nil || apic->addr != 0 || apic->machno == 0)
			continue;
		nproc++;
		if(nproc == MACHMAX){
			print("sipi: MACHMAX too small %d\n", nproc);
			break;
		}

		/*
		 * NOTE: for now, share the page tables with the
		 * bootstrap processor, until the lsipi code is worked out,
		 * so only the Mach and stack portions are used below.
		 */
		alloc = mallocalign(MACHSTKSZ+4*PTSZ+4*KiB+MACHSZ, 4096, 0, 0);
		if(alloc == nil)
			panic("sipi: no memory");
		p = alloc+MACHSTKSZ;

		sipiptr[-1] = mmuphysaddr(PTR2UINT(p));
		DBG("p %#p sipiptr[-1] %#ux\n", p, sipiptr[-1]);

		p += 4*PTSZ+4*KiB;

		/*
		 * Committed. If the AP startup fails, can't safely
		 * release the resources, who knows what mischief
		 * the AP is up to. Perhaps should try to put it
		 * back into the INIT state?
		 */
		mach = (Mach*)p;
		mach->machno = apic->machno;		/* NOT one-to-one... */
		mach->splpc = PTR2UINT(squidboy);
		mach->apicno = apicno;
		mach->stack = PTR2UINT(alloc);
		mach->vsvm = alloc+MACHSTKSZ+4*PTSZ;

		p = KADDR(0x467);
		*p++ = sipipa;
		*p++ = sipipa>>8;
		*p++ = 0;
		*p = 0;

		nvramwrite(0x0f, 0x0a);
		lapicsipi(apicno, sipipa);

		for(i = 0; i < 5000; i += 5){
			if(mach->online)
				break;
			delay(5);
		}
		nvramwrite(0x0f, 0x00);

		DBG("mach %#p (%#p) apicid %d machno %2d %dMHz\n",
			mach, sys->machptr[mach->machno],
			apicno, mach->machno, mach->cpumhz);
	}
}
