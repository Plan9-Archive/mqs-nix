#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "mp.h"
#include "apic.h"
#include "acpi.h"

extern	Madt	*apics;

int
mpacpi(int maxmach)
{
	char *already;
	int np, bp;
	Apic *apic;
	Apicst *st;

	if(apics == nil)
		return maxmach;

	print("apic lapic paddr %#.8P, flags %#.8ux\n",
		apics->lapicpa, apics->pcat);
	np = 0;
	for(st = apics->st; st != nil; st = st->next){
		already = "";
		switch(st->type){
		case ASlapic:
			bp = (np++ == 0);
			if(lapiclookup(st->lapic.id) == nil)
				already = "(mp)";
			else
				lapicinit(st->lapic.id, apics->lapicpa, bp);
			USED(already);
			DBG("lapic: mach %d/%d lapicid %d %s\n", np-1, apic->machno, st->lapic.id, already);
			break;
		case ASioapic:
			if((apic = ioapiclookup(st->ioapic.id)) != nil){
				apic->ibase = st->ioapic.ibase;	/* gnarly */
				already = "(mp)";
			}else{
				apic = ioapicinit(st->ioapic.id, st->ioapic.ibase, st->ioapic.addr);
				if(apic == nil)
					continue;
			}
			print("ioapic: %d ", st->ioapic.id);
			print("addr %p base %d %s\n", apic->paddr, apic->ibase, already);
			break;
		}
	}
	return maxmach - np;
}
