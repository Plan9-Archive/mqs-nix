#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "adr.h"

#include "apic.h"
#include <aml.h>

extern int pcibusno(void*);

static void
memtr(char *s, uintmem pa, int len, void *data)
{
	if(len <= 8)
		print("aml: %s %#P len %d %#llux\n", s, pa, len, getle(data, len));
	else
		print("aml: %s %#P len %d\n", s, pa, len);
}

static uint
seglen(uintmem pa, uintmem len, uintmem sgsz)
{
	uintmem end, slen;
	
	end = pa+sgsz & ~(sgsz-1);
	slen = end - pa;
	if(slen > len)
		slen = len;
	return slen;
}

static int
readmem(Amlio *io, void *data, int len, int off)
{
	uchar *va, *d;
	uintmem s, slen;

	s = io->off+off;
	d = data;
	for(; len != 0; len -= slen){
		slen = seglen(s, len, PGSZ);
		va = tmpmap(s);
		memmove(d, va, slen);
		tmpunmap(va);
		s += slen;
		d += slen;
	}
	memtr("readmem", io->off+off, len, data);
	return len;
}

static int
writemem(Amlio *io, void *data, int len, int off)
{
	uchar *va, *s;
	uintmem d, slen;

	d = io->off+off;
	s = data;
	for(; len != 0; len -= slen){
		slen = seglen(d, len, PGSZ);
		va = tmpmap(d);
		memmove(va, s, slen);
		tmpunmap(va);
		s += slen;
		d += slen;
	}
	memtr("writemem", io->off+off, len, data);
	return len;
}

static int
pciaddr(void *dot)
{
	int adr, bno;
	void *x;

	for(;;){
		if((x = amlwalk(dot, "_ADR")) == nil){
			x = amlwalk(dot, "^");
			if(x == nil || x == dot)
				break;
			dot = x;
			continue;
		}
		if((bno = pcibusno(x)) < 0)
			break;
		if((x = amlval(x)) == nil)
			break;
		adr = amlint(x);
		return MKBUS(BusPCI, bno, adr>>16, adr&0xFFFF);
	}
	return -1;
}

static int
readpcicfg(Amlio *io, void *data, int n, int offset)
{
	uchar *a;
	int i;
	uint r;
	Pcidev *p;

	a = data;
	p = io->aux;
	if(p == nil)
		return -1;
	offset += io->off;
	if(offset > 256)
		return 0;
	if(n+offset > 256)
		n = 256-offset;
	r = offset;
	if(!(r & 3) && n == 4){
		putle(a, pcicfgr32(p, r), 4);
		return 4;
	}
	if(!(r & 1) && n == 2){
		putle(a, pcicfgr16(p, r), 2);
		return 2;
	}
	for(i = 0; i <  n; i++){
		putle(a, pcicfgr8(p, r), 1);
		a++;
		r++;
	}
	return i;
}

static int
writepcicfg(Amlio *io, void *data, int n, int offset)
{
	uchar *a;
	int i;
	uint r;
	Pcidev *p;

	a = data;
	p = io->aux;
	if(p == nil)
		return -1;
	offset += io->off;
	if(offset > 256)
		return 0;
	if(n+offset > 256)
		n = 256-offset;
	r = offset;
	if(!(r & 3) && n == 4){
		pcicfgw32(p, r, getle(a, 4));
		return 4;
	}
	if(!(r & 1) && n == 2){
		pcicfgw16(p, r, getle(a, 2));
		return 2;
	}
	for(i = 0; i <  n; i++){
		pcicfgw8(p, r,  getle(a, 1));
		a++;
		r++;
	}
	return i;
}

enum {
	Post	= 0x80,
};

static int
readioport(Amlio *io, void *data, int len, int port)
{
	uchar *a;

	a = data;
	port += io->off;
	switch(len){
	default:
		return -1;
	case 4:
		putle(a, inl(port), 4);
		break;
	case 2:
		putle(a, ins(port), 2);
		break;
	case 1:
		putle(a, inb(port), 1);
		break;
	}
	if(port != Post)
		print("aml: io read: %ux len %d %llux\n", port, len, getle(a, len));
	return len;
}

static int
writeioport(Amlio *io, void *data, int len, int port)
{
	uchar *a;

	a = data;
	port += io->off;
	if(port != Post)
		print("aml: io write: %ux len %d %llux\n", port, len, getle(a, len));
	switch(len){
	default:
		return -1;
	case 4:
		outl(port, getle(a, 4));
		return 4;
	case 2:
		outs(port, getle(a, 2));
		return 2;
	case 1:
		outb(port, getle(a, 1));
		if(port == Post)
			cgapost(*a);
		return 1;
	}
}

int
amlmapio(Amlio *io)
{
	char buf[64];
	int tbdf;
	Pcidev *p;

	switch(io->space){
	default:
		print("amlmapio: address space %d not implemented\n", io->space);
		break;
	case MemSpace:
		print("aml: map [%#llux, %#llux)\n", io->off, io->off+io->len);
	//	adrmapck(io->off, io->len, Aacpireclaim, Mfree, Cnone);
		io->read = readmem;
		io->write = writemem;
		return 0;
	case IoSpace:
		snprint(buf, sizeof(buf), "%N", io->name);
	//	if(ioalloc(io->off, io->len, 0, buf) < 0){
	//		print("amlmapio: ioalloc failed [%llux-%llux)\n", io->off, io->off+io->len);
	//	//	break;
	//	}
		io->read = readioport;
		io->write = writeioport;
		return 0;
	case PcicfgSpace:
		if((tbdf = pciaddr(io->name)) < 0){
			print("amlmapio: no address: %N\n", io->name);
			break;
		}
		if((p = pcimatchtbdf(tbdf)) == nil){
			print("amlmapio: no device %T\n", tbdf);
			break;
		}
		io->aux = p;
		io->read = readpcicfg;
		io->write = writepcicfg;
		return 0;
	}
	print("amlmapio: mapping %N failed\n", io->name);
	return -1;
}

void
amlunmapio(Amlio *io)
{
	switch(io->space){
	case MemSpace:
		break;
	case IoSpace:
	//	iofree(io->off);
		break;
	}
}

void*
amlalloc(usize n)
{
	void *p;

	if((p = malloc(n)) == nil)
		panic("amlalloc: no memory");
	memset(p, 0, n);
	setmalloctag(&p, getcallerpc(&n));
	setrealloctag(&p, 0);
	return p;
}

void
amlfree(void *p)
{
	free(p);
}

void
amlmicrodelay(int µs)
{
	microdelay(µs);
}
