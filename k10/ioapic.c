#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "apic.h"
#include "io.h"
#include "adr.h"

typedef struct Rbus Rbus;
typedef struct Rdt Rdt;

struct Rbus {
	Rbus	*next;
	int	bustype;
	int	devno;
	Rdt	*rdt;
};

struct Rdt {
	Apic	*apic;
	int	intin;
	u32int	lo;

	u64int	vec;				/* remember vector and mach */
	int	affinity;

	int	ref;				/* could map to multiple busses */
	int	enabled;				/* times enabled */
};

enum {						/* IOAPIC registers */
	Ioregsel	= 0x00,			/* indirect register address */
	Iowin		= 0x04,			/* indirect register data */
	Ioipa		= 0x08,			/* IRQ Pin Assertion */
	Ioeoi		= 0x10,			/* EOI */

	Ioapicid	= 0x00,			/* Identification */
	Ioapicver	= 0x01,			/* Version */
	Ioapicarb	= 0x02,			/* Arbitration */
	Ioabcfg		= 0x03,			/* Boot Coniguration */
	Ioredtbl	= 0x10,			/* Redirection Table */
};

static	Rdt	rdtarray[Nrdt];
static	int	nrdtarray;
static	Rbus*	rdtbus[Nbus];
static	Rdt*	rdtvecno[IdtMAX+1];

static	Apic	xioapic[Napic];
static	int	isabusno = -1;

/* BOTCH: no need for this concept; we've got the bustype */
static void
ioapicisabus(int busno)
{
	if(isabusno != -1){
		if(busno == isabusno)
			return;
		print("ioapic: isabus redefined: %d ↛ %d\n", isabusno, busno);
//		return;
	}
	DBG("ioapic: isa busno %d\n", busno);
	isabusno = busno;
}

Apic*
ioapiclookup(uint id)
{
	Apic *a;

	if(id > nelem(xioapic))
		return nil;
	a = xioapic + id;
	if(a->useable)
		return a;
	return nil;
}

int
gsitoapicid(int gsi, uint *intin)
{
	int i;
	Apic *a;

	for(i=0; i<Napic; i++){
		a = xioapic + i;
		if(!a->useable)
			continue;
		if(gsi >= a->ibase && gsi < a->ibase+a->nrdt){
			if(intin != nil)
				*intin = gsi - a->ibase;
			return a - xioapic;
		}
	}
//	print("gsitoapicid: no ioapic found for gsi %d\n", gsi);
	return -1;
}

static void
rtblget(Apic* apic, int sel, u32int* hi, u32int* lo)
{
	sel = Ioredtbl + 2*sel;

	apic->addr[Ioregsel] = sel+1;
	*hi = apic->addr[Iowin];
	apic->addr[Ioregsel] = sel;
	*lo = apic->addr[Iowin];
}

static void
rtblput(Apic* apic, int sel, u32int hi, u32int lo)
{
	sel = Ioredtbl + 2*sel;

	apic->addr[Ioregsel] = sel+1;
	apic->addr[Iowin] = hi;
	apic->addr[Ioregsel] = sel;
	apic->addr[Iowin] = lo;
}

Rdt*
rdtlookup(Apic *apic, int intin)
{
	int i;
	Rdt *r;

	for(i = 0; i < nrdtarray; i++){
		r = rdtarray + i;
		if(apic == r->apic && intin == r->intin)
			return r;
	}
	return nil;
}

void
ioapicintrinit(int bustype, int busno, int apicno, int intin, int devno, u32int lo)
{
	Rbus *rbus;
	Rdt *rdt;
	Apic *apic;

	if(busno >= Nbus || apicno >= Napic || nrdtarray >= Nrdt)
		return;

	if(bustype == BusISA)
		ioapicisabus(busno);

	apic = &xioapic[apicno];
	if(!apic->useable || intin >= apic->nrdt)
		panic("ioapic: intrinit: usable %d nrdt %d: bus %d apic %d intin %d dev %d lo %.8ux\n",
			apic->useable, apic->nrdt, busno, apicno, intin, devno, lo);

	rdt = rdtlookup(apic, intin);
	if(rdt == nil){
		if(nrdtarray == nelem(rdtarray)){
			print("ioapic: intrinit: rdtarray too small\n");
			return;
		}
		rdt = &rdtarray[nrdtarray++];
		rdt->apic = apic;
		rdt->intin = intin;
		rdt->lo = lo;
	}else{
		if(lo != rdt->lo){
			if(bustype == BusISA && intin < 16)
			if(lo == (Im|IPhigh|TMedge)){
				DBG("override: isa %d\n", intin);
				return;	/* expected; default was overridden*/
			}
			print("mutiple irq botch type %d bus %d %d/%d/%d lo %.8ux vs %.8ux\n",
				bustype, busno, apicno, intin, devno, lo, rdt->lo);
			return;
		}
		DBG("dup rdt %d %d %d %d %.8ux\n", busno, apicno, intin, devno, lo);
	}
	rdt->ref++;
	rbus = malloc(sizeof *rbus);
	rbus->rdt = rdt;
	rbus->bustype = bustype;
	rbus->devno = devno;
	rbus->next = rdtbus[busno];
	rdtbus[busno] = rbus;
}

/*
 * deal with ioapics at the same physical address.  seen on
 * certain supermicro atom systems.  the hope is that only
 * one will be used, and it will be the second one initialized.
 * (the pc kernel ignores this issue.)  it could be that mp and
 * acpi have different numbering?
 */
static Apic*
dupaddr(uintmem pa)
{
	int i;
	Apic *p;

	for(i = 0; i < nelem(xioapic); i++){
		p = xioapic + i;
		if(p->paddr == pa)
			return p;
	}
	return nil;
}

Apic*
ioapicinit(int id, int ibase, uintmem pa)
{
	Apic *apic, *p;
	static int base;

	/*
	 * Mark the IOAPIC useable if it has a good ID
	 * and the registers can be mapped.
	 */
	if(id >= Napic)
		return nil;
	if((apic = xioapic+id)->useable)
		return apic;

	if((p = dupaddr(pa)) != nil){
		print("ioapic%d: same pa as apic%ld\n", id, p-xioapic);
		if(ibase != -1)
			return nil;		/* mp irqs reference mp apic#s */
		apic->addr = p->addr;
	}
	else{
		adrmapck(pa, 1024, Ammio, Mfree, Cnone);	/* not in adr? */
		if((apic->addr = vmap(pa, 1024)) == nil){
			print("ioapic%d: can't vmap %#P\n", id, pa);
			return nil;
		}
	}
	apic->useable = 1;
	apic->paddr = pa;

	/*
	 * Initialise the I/O APIC.
	 * The MultiProcessor Specification says it is the
	 * responsibility of the O/S to set the APIC ID.
	 */
	lock(apic);
	apic->addr[Ioregsel] = Ioapicver;
	apic->nrdt = (apic->addr[Iowin]>>16 & 0xff) + 1;
	if(ibase != -1)
		apic->ibase = ibase;
	else{
		apic->ibase = base;
		base += apic->nrdt;
	}
	apic->addr[Ioregsel] = Ioapicid;
	apic->addr[Iowin] = id<<24;
	unlock(apic);

	return apic;
}

void
ioapicdump(void)
{
	int i, n;
	Rbus *rbus;
	Rdt *rdt;
	Apic *apic;
	u32int hi, lo;

	if(!DBGFLG)
		return;
	for(i = 0; i < Napic; i++){
		apic = &xioapic[i];
		if(!apic->useable || apic->addr == 0)
			continue;
		print("ioapic %d addr %#p nrdt %d ibase %d\n",
			i, apic->addr, apic->nrdt, apic->ibase);
		for(n = 0; n < apic->nrdt; n++){
			lock(apic);
			rtblget(apic, n, &hi, &lo);
			unlock(apic);
			print(" rdt %2.2d %#8.8ux %#8.8ux\n", n, hi, lo);
		}
	}
	for(i = 0; i < Nbus; i++){
		if((rbus = rdtbus[i]) == nil)
			continue;
		print("iointr bus %d:\n", i);
		for(; rbus != nil; rbus = rbus->next){
			rdt = rbus->rdt;
			print(" apic %ld devno %#ux (%d %d) intin %d lo %#ux ref %d\n",
				rdt->apic-xioapic, rbus->devno, rbus->devno>>2,
				rbus->devno & 0x03, rdt->intin, rdt->lo, rdt->ref);
		}
	}
}

static char*
ioapicprint(char *p, char *e, Ioapic *a, int i)
{
	char *s;

	s = "ioapic";
	p = seprint(p, e, "%-8s ", s);
	p = seprint(p, e, "%8ux ", i);
	p = seprint(p, e, "%6d ", a->ibase);
	p = seprint(p, e, "%6d ", a->ibase+a->nrdt-1);
	p = seprint(p, e, "%#P ", a->paddr);
	p = seprint(p, e, "\n");
	return p;
}

static long
ioapicread(Chan*, void *a, long n, vlong off)
{
	char *s, *e, *p;
	long i, r;

	s = malloc(READSTR);
	e = s+READSTR;
	p = s;

	for(i = 0; i < nelem(xioapic); i++)
		if(xioapic[i].useable)
			p = ioapicprint(p, e, xioapic + i, i);
	r = -1;
	if(!waserror()){
		r = readstr(off, a, n, s);
		poperror();
	}
	free(s);
	return r;
}

void
ioapiconline(void)
{
	int i;
	Apic *apic;

	addarchfile("ioapic", 0444, ioapicread, nil);
	for(apic = xioapic; apic < &xioapic[Napic]; apic++){
		if(!apic->useable || apic->addr == nil)
			continue;
		for(i = 0; i < apic->nrdt; i++){
			lock(apic);
			rtblput(apic, i, 0, Im);
			unlock(apic);
		}
	}
	ioapicdump();
}

/*
 * pick a lapic with an active mach by round-robin
 */
static Mach*
selmach(void)
{
	Apic *lapic;
	Mach *mach;
	static int i;

	for(;; i = (i+1) % Napic){
		if((lapic = lapiclookup(i)) == nil)
			continue;
		if((mach = sys->machptr[lapic->machno]) == nil)
			continue;
		if(mach->online){
			i++;
			return mach;
		}
	}
}

static int
selmachvec(Mach *mach)
{
	uchar *v;
	uint vecno;
	Lapic *lapic;

	lapic = lapiclookup(mach->apicno);
	v = lapic->vecalloc;

	lock(&lapic->vecalloclk);
	for(vecno = IdtIOAPIC; vecno <= IdtMAX; vecno += 8)
		if(v[vecno/8] != 0xff)
			break;
	if(vecno > IdtMAX){
		unlock(&lapic->vecalloclk);
		return -1;
	}
	while((v[vecno/8] & 1<<vecno%8) != 0)
		vecno++;
	v[vecno/8] |= 1<<vecno%8;
	unlock(&lapic->vecalloclk);
	return vecno;
}

static int
pickmachvec(Vctl *v, Mach **mach)
{
	uint vno, i;

	if(v->affinity != -1){
		*mach = sys->machptr[v->affinity];
		return selmachvec(*mach);
	}
	for(i = 0; i < sys->nmach; i++)
		if((vno = selmachvec(*mach = selmach())) != -1){
			v->affinity = (*mach)->machno;
			return vno;
		}
	return -1;
}

int
ioapicphysdd(Vctl *v, u32int* hi, u32int* lo)
{
	Mach *mach;

	if((v->vno = pickmachvec(v, &mach)) == -1)
		return -1;
	/* Set delivery mode (lo) and destination field (hi) */
	*hi = mach->apicno<<24;
	*lo |= v->vno|Pm|MTf;
	if(*lo & Lm)
		*lo |= MTlp;
	return 0;
}

static int
msimask(Vkey *v, int mask)
{
	Pcidev *p;

	p = pcimatchtbdf(v->tbdf);
	if(p == nil)
		return -1;
	return pcimsimask(p, mask);
}

static int
intrenablemsi(Vctl* v, Pcidev *p)
{
	u32int lo, hi;
	u64int msivec;

	lo = IPlow | TMedge;
	ioapicphysdd(v, &hi, &lo);

	msivec = (u64int)hi<<32 | lo;
	if(pcimsienable(p, msivec) == -1)
		return -1;
	v->eoi = lapiceoi;
	v->type = "msi";
	v->mask = msimask;

	DBG("msiirq: %T: enabling %.16llux %s vno %d\n", p->tbdf, msivec, v->name, v->vno);
	return v->vno;
}

int
disablemsi(Vctl*, Pcidev *p)
{
	if(p == nil)
		return -1;
	return pcimsimask(p, 1);
}

int
ioapicintrenable(Vctl* v)
{
	Pcidev *p;
	Rbus *rbus;
	Rdt *rdt;
	u32int hi, lo;
	int bustype, busno, devno;

	if(v->tbdf == BUSUNKNOWN){
		if(v->irq >= IrqLINT0 && v->irq <= MaxIrqLAPIC){
			if(v->irq != IrqSPURIOUS)
				v->isr = lapiceoi;
			v->type = "lapic";
			return v->irq;
		}
		else{
			/*
			 * Legacy ISA.
			 * Make a busno and devno using the
			 * ISA bus number and the irq.
			 */
			if(isabusno == -1)
				panic("no ISA bus allocated");
			busno = isabusno;
			devno = v->irq;
			bustype = BusISA;
		}
	}
	else if((bustype = BUSTYPE(v->tbdf)) == BusPCI){
		busno = BUSBNO(v->tbdf);
		if((p = pcimatchtbdf(v->tbdf)) == nil)
			panic("ioapic: no pci dev for tbdf %T", v->tbdf);
		if(intrenablemsi(v, p) != -1)
			return v->vno;
		disablemsi(v, p);
		if((devno = pcicfgr8(p, PciINTP)) == 0)
			panic("no INTP for tbdf %T", v->tbdf);
		devno = BUSDNO(v->tbdf)<<2|(devno-1);
		DBG("ioapicintrenable: tbdf %T busno %d devno %d\n",
			v->tbdf, busno, devno);
	}
	else{
		SET(busno, devno);
		panic("unknown tbdf %T", v->tbdf);
	}

	rdt = nil;
	for(rbus = rdtbus[busno]; rbus != nil; rbus = rbus->next)
		if(rbus->devno == devno && rbus->bustype == bustype){
			rdt = rbus->rdt;
			break;
		}
	if(rdt == nil){
		/*
		 * First crack in the smooth exterior of the new code:
		 * some BIOS make an MPS table where the PCI devices are
		 * just defaulted to ISA.
		 * Rewrite this to be cleaner.
		 */
		if((busno = isabusno) == -1)
			return -1;
		devno = v->irq<<2;
		for(rbus = rdtbus[busno]; rbus != nil; rbus = rbus->next)
			if(rbus->devno == devno){
				rdt = rbus->rdt;
				break;
			}
		DBG("isa: tbdf %T busno %d devno %d %#p\n",
			v->tbdf, busno, devno, rdt);
	}
	if(rdt == nil)
		return -1;

	/*
	 * Assume this is a low-frequency event so just lock
	 * the whole IOAPIC to initialise the RDT entry
	 * rather than putting a Lock in each entry.
	 */
	DBG("%T: %ld/%d/%d (%d)\n", v->tbdf, rdt->apic - xioapic, rbus->devno, rdt->intin, devno);

	lock(rdt->apic);
	ainc(&rdt->enabled);
	lo = (rdt->lo & ~Im);

	if((rdt->lo & 0xff) == 0){
		ioapicphysdd(v, &hi, &lo);
		rdt->lo |= lo & 0xff;
		rdt->vec = (u64int)hi<<32 | lo;
		rdt->affinity = v->affinity;
		rdtvecno[lo&0xff] = rdt;
		rtblput(rdt->apic, rdt->intin, hi, lo);
	}else{
		DBG("%T: mutiple irq bus %d dev %d %s\n", v->tbdf, busno, devno, v->name);
		hi = rdt->vec>>32;
		lo = rdt->vec;
		v->affinity = rdt->affinity;
		v->vno = lo & 0xff;
	}
	unlock(rdt->apic);

	DBG("busno %d devno %d hi %#.8ux lo %#.8ux vno %d af %d\n",
		busno, devno, hi, lo, v->vno, v->affinity);
	v->eoi = lapiceoi;
	v->type = "ioapic";

	return v->vno;
}

int
ioapicintrdisable(int vecno)
{
	Rdt *rdt;

	if((rdt = rdtvecno[vecno]) == nil){
		panic("ioapicintrdisable: vecno %d has no rdt", vecno);
		return -1;
	}

	lock(rdt->apic);
	if(adec(&rdt->enabled) == 0)
		rtblput(rdt->apic, rdt->intin, 0, rdt->lo);
	unlock(rdt->apic);

	return 0;
}
