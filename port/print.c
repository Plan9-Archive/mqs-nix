#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

static Lock fmtl;

void
_fmtlock(void)
{
	lock(&fmtl);
}

void
_fmtunlock(void)
{
	unlock(&fmtl);
}

int
_efgfmt(Fmt*)
{
	return -1;
}

int
errfmt(Fmt*)
{
	return -1;
}

int
fmtP(Fmt* f)
{
	uintmem pa;

	pa = va_arg(f->args, uintmem);

	if(sizeof(uintmem) <= 4 /* || (pa & ~(uintmem)0xffffffff) == 0 */){
		if(f->flags & FmtSharp)
			return fmtprint(f, "%#.8ux", (u32int)pa);
		return fmtprint(f, "%ud", (u32int)pa);
	}

	if(f->flags & FmtSharp)
		return fmtprint(f, "%#.16llux", (u64int)pa);
	return fmtprint(f, "%llud", (u64int)pa);
}

void
fmtinit(void)
{
	fmtinstall('P', fmtP);
	quotefmtinstall();
}
