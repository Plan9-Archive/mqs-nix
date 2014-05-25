#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	<a.out.h>

static ulong
l2be(long l)
{
	uchar *cp;

	cp = (uchar*)&l;
	return (cp[0]<<24) | (cp[1]<<16) | (cp[2]<<8) | cp[3];
}


static void
readn(Chan *c, void *vp, long n)
{
	char *p;
	long nn;

	p = vp;
	while(n > 0) {
		nn = c->dev->read(c, p, n, c->offset);
		if(nn == 0)
			error(Eshort);
		c->offset += nn;
		p += nn;
		n -= nn;
	}
}

static void
setbootcmd(int argc, char *argv[])
{
	char *buf, *p, *ep;
	int i;

	buf = malloc(1024);
	if(buf == nil)
		error(Enomem);
	p = buf;
	ep = buf + 1024;
	for(i=0; i<argc; i++)
		p = seprint(p, ep, "%q ", argv[i]);
	*p = 0;
	ksetenv("bootcmd", buf, 1);
	free(buf);
}

static void
checkmagic(ulong magic)
{
	int i;
	ulong except[] = {S_MAGIC, I_MAGIC, ~0, ~0, };

	for(i = 0;; i++)
		if(AOUT_MAGIC == except[i] || except[i] == ~0)
			break;
	if(magic != AOUT_MAGIC && magic != except[i^1])
		error(Ebadexec);
}

void
rebootcmd(int argc, char *argv[])
{
	Chan *c;
	Exec exec;
	ulong magic, text, rtext, entry, data, size;
	uchar *p, ulv[8];

	if(argc == 0)
		exit(0);

	c = namec(argv[0], Aopen, OREAD, 0);
	if(waserror()){
		cclose(c);
		nexterror();
	}

	readn(c, &exec, sizeof(Exec));
	magic = l2be(exec.magic);
	entry = l2be(exec.entry);
	text = l2be(exec.text);
	data = l2be(exec.data);

	checkmagic(magic);

	if(magic & HDR_MAGIC)
		readn(c, ulv, 8);
	/* round text out to page boundary */
	rtext = ROUNDUP(entry+text, PGSZ) - entry;
	size = rtext + data;
	p = malloc(size);
	if(p == nil)
		error(Enomem);

	if(waserror()){
		free(p);
		nexterror();
	}

	memset(p, 0, size);
	readn(c, p, text);
	readn(c, p + rtext, data);

	ksetenv("bootfile", argv[0], 1);
	setbootcmd(argc-1, argv+1);

	reboot((void*)entry, p, size);
	panic("return from reboot!");

	poperror();
	free(p);

	cclose(c);
	poperror();
}
