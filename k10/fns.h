#include "../port/portfns.h"
void	acpiinit(int);
Dirtab*	addarchfile(char*, int, long(*)(Chan*, void*, long, vlong), long(*)(Chan*, void*, long, vlong));
void	adrinit(void);
void	apicipi(int);
void	apicpri(int);
void	apmmuinit(void);
vlong	archhz(void);
void	archinit(void);
int	archmmu(void);
void	archreset(void);
#define	BIOSSEG(a)	KADDR(((uint)(a))<<4)
void	cgaconsputs(char*, int);
void	cgainit(void);
void	cgapost(int);
void	checkpa(char*, uintmem);
#define	clearmmucache()				/* x86 doesn't have one */
#define	coherence()	mfence()
void	confsetenv(void);
void	cpuid(u32int, u32int, u32int[4]);
void	delay(int);
void	devacpiinit(void);
void	dumpmmu(Proc*);
void	dumpmmuwalk(uintmem);
void	dumpptepg(int, uintmem);
int	fpudevprocio(Proc*, void*, long, vlong, int);
void	fpuinit(void);
void	fpunoted(void);
void	fpunotify(Ureg*);
void	fpuprocrestore(Proc*);
void	fpuprocsave(Proc*);
void	fpusysprocsetup(Proc*);
void	fpusysrfork(Ureg*);
void	fpusysrforkchild(Proc*, Proc*);
void	fpusysrfork(Ureg*);
void	gdtget(void*);
void	gdtput(int, u64int, u16int);
char*	getconf(char*);
u64int	getcr0(void);
u64int	getcr2(void);
u64int	getcr3(void);
u64int	getcr4(void);
void	halt(void);
void	hardhalt(void);
int	i8042auxcmd(int);
void	i8042auxenable(void (*)(int));
void	i8042kbdenable(void);
void	i8042reset(void);
void*	i8250alloc(int, int, int);
void	i8250console(void);
int	i8259init(int);
void	idlehands(void);
void	idthandlers(void);
void	idtput(int, u64int);
int	inb(int);
u32int	inl(int);
void	insb(int, void*, int);
ushort	ins(int);
void	insl(int, void*, int);
void	inss(int, void*, int);
int	intrdisable(void*);
void*	intrenable(int, void (*)(Ureg*, void*), void*, int, char*);
void	invlpg(uintptr);
int	ioalloc(int, int, int, char*);
void	iofree(int);
void	ioinit(void);
int	ioreserve(int, int, int, char*);
int	iounused(int, int);
int	islo(void);
void*	KADDR(uintmem);
void	kbdenable(void);
void	kbdinit(void);
void	kexit(Ureg*);
#define	kmapinval()
void	lfence(void);
void	links(void);
void	mach0init(void);
void	machinit(void);
void	meminit(void);
void	mfence(void);
void	mmumap(uintmem, uintptr, u64int, uint);
void	mmuflushtlb(uintmem);
void	mmuinit(void);
void	mmumap(uintmem, uintptr, u64int, uint);
uintmem	mmuphysaddr(uintptr);
int	mmuwalk(PTE*, uintptr, int, PTE**, uintmem (*)(usize));
int	mpacpi(int);
void	mpsinit(int);
void	ndnr(void);
void	nmienable(void);
void	noerrorsleft(void);
uchar	nvramread(int);
void	nvramwrite(int, uchar);
void	optionsinit(char*);
void	outb(int, int);
void	outl(int, u32int);
void	outsb(int, void*, int);
void	outs(int, ushort);
void	outsl(int, void*, int);
void	outss(int, void*, int);
uintmem	PADDR(void*);
void	pause(void);
void	physallocdump(void);
void	printcpufreq(void);
void	putcr0(u64int);
void	putcr3(u64int);
void	putcr4(u64int);
u64int	rdmsr(u32int);
void	rdrandbuf(void*, usize);
u64int	rdtsc(void);
int	screenprint(char*, ...);			/* debugging */
void	sfence(void);
void	sipi(void);
void	syscallentry(void);
void	syscallreturn(void);
void	syscall(uint, Ureg*);
void	sysrforkret(void);
void*	tmpmap(uintmem);
void	tmpunmap(void*);
void	touser(uintptr);
void	trapenable(int, void (*)(Ureg*, void*), void*, char*);
void	trapinit(void);
void	trap(Ureg*);
void	trput(u64int);
void	tssrsp0(u64int);
void	umeminit(void);
int	userureg(Ureg*);
#define	validalign(adr, sz)				/* x86 doesn't care */
void	vctlinit(Vctl*);
void*	vintrenable(Vctl*, char*);
void*	vmapoverlap(uintmem, usize);
void*	vmappat(uintmem, usize, uint);
int	vmapsync(uintptr);
void*	vmap(uintmem, usize);
void	vsvminit(int);
void	vunmap(void*, usize);
void	writeconf(void);
void	wrmsr(u32int, u64int);
#define PTR2UINT(p)	((uintptr)(p))
#define UINT2PTR(i)	((void*)(i))

int	ainc8(void*);
int	cas32(void*, u32int, u32int);
int	cas64(void*, u64int, u64int);
u32int	fas32(u32int*, u32int);
u64int	fas64(u64int*, u64int);
int	tas32(void*);

#define	cas(p, e, n)	cas32((p), (u32int)(e), (u32int)(n))
#define casp(p, e, n)	cas64((p), (u64int)(e), (u64int)(n))
#define	fas(p, v)		((int)fas32((u32int*)(p), (u32int)(v)))
#define	fasp(p, v)	((void*)fas64((u64int*)(p), (u64int)(v)))
#define tas(p)		tas32(p)

#define	monmwait(v, o)	((int)monmwait32((u32int*)(v), (u32int)(o)))
u32int	(*monmwait32)(u32int*, u32int);
u32int	nopmonmwait32(u32int*, u32int);
u32int	k10monmwait32(u32int*, u32int);

#define	monmwaitp(v, o)	monmwait64(v, o);
u64int	(*monmwait64)(u64int*, u64int);
u64int	nopmonmwait64(u64int*, u64int);
u64int	k10monmwait64(u64int*, u64int);

#pragma		varargck	argpos	screenprint		1
