#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "adr.h"
#include "io.h"
#include "apic.h"

u64int (*monmwait64)(u64int*, uintptr) = nopmonmwait64;
u32int (*monmwait32)(u32int*, u32int) = nopmonmwait32;

u64int
nopmonmwait64(u64int *p, u64int v)
{
	if(*(u64int*)p == v)
		pause();
	return *(u64int*)p;
}

u32int
nopmonmwait32(u32int *p, u32int v)
{
	if(*(u32int*)p == v)
		pause();
	return *(u32int*)p;
}

void
enableextpci(void)
{
	u32int info[4];
	u64int msr;

	cpuid(0, 0, info);
	if(memcmp(&info[1], "AuthcAMDenti", 12) == 0){
		cpuid(1, 0, info);
		switch(info[0] & 0x0fff0ff0){
		case 0x00100f40:		/* phenom ii x4 */
		case 0x00100f90:		/* K10 */
		case 0x00000620:		/* QEMU64 */
		case 0x00600f20:		 /* steamroller fx-8320 */
			msr = rdmsr(0xc001001f);
			msr |= 1ull<<46;	/* allow extended pci access */
			wrmsr(0xc001001f, msr);
		}
	}
}

static void
k10archinit(void)
{
	u32int info[4];

	cpuid(1, 0, info);
	if(info[2] & 8){
		monmwait64 = k10monmwait64;
		monmwait32 = k10monmwait32;
	}
	enableextpci();
}

static int
brandstring(char *s)
{
	char *p;
	int i, j;
	u32int r[4];

	p = s;
	for(i = 0; i < 3; i++){
		cpuid(0x80000002+i, 0, r);
		for(j = 0; j < 4; j++){
			memmove(p, r+j, 4);
			p += 4;
		}
	}
	*p = 0;
	return 0;
}

/* use intel brand string to discover hz */
static vlong
intelbshz(void)
{
	char s[4*4*3+1], *h;
	uvlong scale;

	brandstring(s);
	DBG("brandstring: %s\n", s);

	h = strstr(s, "Hz");		/* 3.07THz */
	if(h == nil || h-s < 5)
		return 0;
	h[2] = 0;

	scale = 1000;
	switch(h[-1]){
	default:
		return 0;
	case 'T':
		scale *= 1000;
	case 'G':
		scale *= 1000;
	case 'M':
		scale *= 1000;
	}

	/* get rid of the fractional part */
	if(h[-4] == '.'){
		h[-4] = h[-5];
		h[-5] = ' ';
		scale /= 100;
	}
	return atoi(h-5)*scale;
}

enum {
	PciADDR		= 0xcf8,
	PciDATA		= 0xcfc,
};

uint
pciread(uint tbdf, int r, int w)
{
	int o, er;

	o = r & 4-w;
	er = r&0xfc | (r & 0xf00)<<16;
	outl(PciADDR, 0x80000000|BUSBDF(tbdf)|er);
	switch(w){
	default:
	case 1:
		return inb(PciDATA+o);
	case 2:
		return ins(PciDATA+o);
	case 4:
		return inl(PciDATA+o);
	}
}

static vlong
cpuidhz(void)
{
	int r;
	vlong hz;
	u32int info[4];
	u64int msr;

	cpuid(0, 0, info);
	if(memcmp(&info[1], "GenuntelineI", 12) == 0){
		hz = intelbshz();
		DBG("cpuidhz: %#llux hz\n", hz);
	}
	else if(memcmp(&info[1], "AuthcAMDenti", 12) == 0){
		cpuid(1, 0, info);
		switch(info[0] & 0x0fff0ff0){
		default:
			iprint("cpuidhz: BUG: HZ unknown for %.8ux: guess 2.5Ghz\n",
				info[0] & 0x0fff0ff0);
			hz = 2500000000;
			break;
		case 0x00000f50:		/* K8 */
			msr = rdmsr(0xc0010042);
			if(msr == 0)
				return 0;
			hz = (800 + 200*((msr>>1) & 0x1f)) * 1000000ll;
			break;
		case 0x00100f40:		/* phenom ii x4 */
		case 0x00100f90:		/* K10 */
		case 0x00000620:		/* QEMU64 */
			msr = rdmsr(0xc0010064);
			r = (msr>>6) & 0x07;
			hz = (((msr & 0x3f)+0x10)*100000000ll)/(1<<r);
			break;
		case 0x00600f20:		 /* steamroller fx-8320 */
			r = MKBUS(BusPCI, 0, 0x18, 4);
			r = pciread(r, 0x15c+4, 4)>>16 & 7;
			if(r != 2){
				print("r != 2 (%d)\n", r);
				r = 2;
			}
			msr = rdmsr(0xc0010064 + r);
			hz = (((msr & 0x3f)+0x10)*100000000ll)/(1<<r);
			break;
		}
		DBG("cpuidhz: %#llux hz %lld\n", msr, hz);
	}
	else
		hz = 0;
	return hz;
}

vlong
archhz(void)
{
	vlong hz;

	assert(m->machno == 0);
	k10archinit();		/* botch; call from archinit */
	hz = cpuidhz();
	if(hz == 0)
		panic("hz 0");
	return hz;
}

static void
addmachpgsz(int bits)
{
	int i;

	i = m->npgsz;
	m->pgszlg2[i] = bits;
	m->pgszmask[i] = (1<<bits)-1;
	m->pgsz[i] = 1<<bits;
	m->npgsz++;
}

int
archmmu(void)
{
	u32int info[4];

	addmachpgsz(12);
	addmachpgsz(21);

	/*
	 * Check the Page1GB bit in function 0x80000001 DX for 1*GiB support.
	 */
	cpuid(0x80000001, 0, info);
	if(info[3] & 0x04000000)
		addmachpgsz(30);
	return m->npgsz;
}

void
microdelay(int µs)
{
	u64int r, t;

	r = rdtsc();
	for(t = r + m->cpumhz*µs; r < t; r = rdtsc())
		pause();
}

void
delay(int ms)
{
	u64int r, t;

	r = rdtsc();
	for(t = r + m->cpumhz*1000ull*ms; r < t; r = rdtsc())
		pause();
}

static void
pcireservemem(void)
{
	int i;
	Pcidev *p;
	
	for(p = nil; p = pcimatch(p, 0, 0); )
		for(i=0; i<nelem(p->mem); i++)
			if(p->mem[i].bar && (p->mem[i].bar&1) == 0)
				adrmapinit(p->mem[i].bar&~(uintmem)0xf, p->mem[i].size, Apcibar, Mfree, Cnone);
}

void
archpciinit(void)
{
	pcireservemem();
}

/* no deposit, no return */
void
ndnr(void)
{
	splhi();
	lapicpri(0xff);
	for(;;)
		hardhalt();
}
