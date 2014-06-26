/*
 * intel 40gbe pcie driver (unfinished)
 * - debugging still enabled
 * - no performance considerations yet.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"
#include "../port/netif.h"
#include "etherif.h"

/*
 * notes to the formerly gentle reader.
 *
 * most of these registers are indexed by pf (physical function)
 * usually the offset is 1 word per pf, but e.g. Ral is an exception.
 *
 * this controller uses the mellanox trick of forcing the driver
 * to arrange page tables for the nic.  this is called "hmc"—
 * “host memory cache”.
 *
 * this controller has an internal ethernet switch with ieee 802.1Qbg
 * compatable features such as s-tags.  a few terms:
 * · veb refers to hardware offload of software virtual bridges.
 * · vsi refers to the "virtual station interface".
 * · evb refers to the "edge virtual bridging".
 * we try to ignore as many of these bits as we can.  but all
 * traffic goes through vsis, so we use the one firmware has
 * provided.
 */
enum {
	Ctl		= 0x092400/4,
	Rxctl		= 0x12a500/4,
	Clkstat		= 0x0b8184/4,

	/* reset */
	Rstctl		= 0x0b8180/4,		/* reset control */
	Rststat		= 0x0b8188/4,		/* " status */
	Rstempen	= 0x0b818c/4,		/* " emp enable */
	Rtrig		= 0x0b8190/4,		/* " trigger */

	/* general */
	Portnum		= 0x1c0480/4,		/* (pf) port number */
	Pfgenctl		= 0x092400/4,		/* pf ctl (+1*portnum) */

	/* shadow ram */
	Srgens		= 0xb6100/4,		/* " general setup */
	Srfla		= 0xb6108/4,		/* " programed mode (ie. blank or not) */
	Srctl		= 0xb6110/4,		/* " control */
	Srdata		= 0xb6114/4,		/* " 16-bit data */

	/* interrupt */
	Icr		= 0x38780/4,
	Icren		= 0x38800/4,
	Idynctl		= 0x38480/4,
	Istatctl		= 0x38400/4,
	Ilnklst		= 0x38500/4,
	Irate		= 0x38580/4,

	Iraten		= 0x35800/4,		/* pf irq n rate limit */
	Irxqctl		= 0x3a000/4,		/* rx q irq cause control */

	Itxqctl		= 0x3c000/4,		/* tx q icc */
	Iasyctl		= 0x38700/4,		/* asynch event q icc */
 
	/* admin queue */
	Atqlo		= 0x080000/4,		/* admin tx queue address low */
	Atqhi		= 0x080100/4,
	Atqlen		= 0x080200/4,		/* admin tx queue len */
	Atqh		= 0x080300/4,		/* firmware: current ptr */
	Atqt		= 0x080400/4,		/* driver: current ptr */
		
	Arqlo		= 0x080080/4,		/* admin tx queue address low */
	Arqhi		= 0x080180/4,
	Arqlen		= 0x080280/4,		/* admin tx queue len */
	Arqh		= 0x080380/4,		/* firmware: current ptr */
	Arqt		= 0x080480/4,		/* driver: current ptr */

	/* rx/rx queues */
	Qalloc		= 0x1c0400/4,		/* queue allocation */

	/* tx */
	Txctl		= 0x104000/4,
	Txen		= 0x100000/4,		/* index by queue# not pf */
	Txtail		= 0x108000/4,		/* index by queue# not pf; Rdt */

	/* rx */
	Ral		= 0x1e4440/4,		/* + 5*portnum */
	Rah		= 0x1e44c0/4,
	Rpadctl		= 0x51060/4,		/* */

	Rxen		= 0x120000/4,		/* index by queue# not pf */
	Rxtail		= 0x128000/4,		/* index by queue# not pf */

	/* vsi (virtual station interface) */
	Vsiqbase		= 0x20c800/4,		/* index by vis# (not id) */
	Vsilanqtable	= 0x200000/4,

	/* host memory cache */
	Hmtxobjsz	= 0xc2004/4,		/* log₂ */
	Hmrxobjsz	= 0xc200c/4,

	Hmsdpart	= 0xc0800/4,
	Hmsdcmd	= 0xc0000/4,		/* relative index */
	Hmsdaddrlo	= 0xc0100/4,
	Hmsdaddrhi	= 0xc0200/4,
	Hminval		= 0xc0300/4,

	Hmerr		= 0xc0400/4,
	Hmerrd		= 0xc0500/4,

	Hmtxbase	= 0xc6200/4,
	Hmtxcnt		= 0xc6300/4,
	Hmrxbase	= 0xc6400/4,
	Hmrxcnt		= 0xc6500/4,

	/* ctx direct programming (cheating) */
	Ctxctl		= 0x10c300/4,
	Ctxstat		= 0x10c380/4,
	Ctxdata		= 0x10c100/4,		/* + 0x20*n + pf */

	/* bit def'n */

	/* Rstctl */
	Eccrsten		= 1<<8,		/* graceful reset on ecc error */

	/* Ctl */
	Sreset		= 1<<0,

	/* Rxctl */
	Pxemode		= 1<<0,

	/* Icr, Icren */
	Intevent		= 1<<0,
	Queue0		= 1<<1,		/* queue n */
	Eccerr		= 1<<16,
	Malice		= 1<<19,		/* malicious driver behavior */
	Grst		= 1<<20,		/* global reset requested */
	Pcierr		= 1<<21,
	Gpio		= 1<<22,
	Timesync	= 1<<23,
	Storm		= 1<<24,
	Linkchange	= 1<<25,		/* not in the datasheet */
	Hmcerr		= 1<<26,		/* cf. pfhmc_errorinfo an pfchmc_errordata */
	Vflr		= 1<<29,		/* vflr initiated by vf */
	Adminq		= 1<<30,
	Swint		= 1<<31,
	Errint		= Eccerr | Malice | Grst | Pcierr | Hmcerr,
	Aprocint		= Errint | Linkchange | Adminq,

	/* Idynctl */
	Intena		= 1<<0,
	Clearpba		= 1<<1,
	Swinttrig		= 1<<2,
	Itridx		= 1<<3,		/* 2 bits */
	Interval		= 1<<5,		/* 12 bits */
	Swinttrigen	= 1<<24,
	Swinttrigidx	= 1<<25,		/* 2 bits */
	Intenamsk	= 1<<31,

	/* Ilnklst */
	Firstqidx		= 1<<10,
	Firstrx		= 0<<12,
	Firsttx		= 1<<12,

	/* Irxqctl, Itxqctl */
	Iqitridx		= 1<<11,		/* itr idx of interrupt cause */
	Iqirqidx		= 1<<13,		/* idx of "queue" bits in Icr, x ∈ [0,7] */
	Iqnxtqidx	= 1<<16,		/* idx of next queue */
	Iqend		= 0x7ff * Iqnxtqidx,
	Iqnxtr		= 0<<27,		/* next queue type is rx */
	Iqnxtt		= 1<<27,		/* " tx */
	Iqnxtpe		= 2<<27,		/* " pe */
	Iqenable		= 1<<30,

	Irxidx		= 1,
	Itxidx		= 0,
	Irx		= 1<<1+Irxidx,		/* software defined */
	Itx		= 1<<1+Itxidx,		/* software defined */

	/* Rststat */
	Devstate		= 3<<0,		/* reset state; 0 means no reset */
	Rsttype		= 3<<2,		/* type of reset requested */

	/* Pfgenctl */
	Pflinken		= 1<<2,		/* enable pf link */

	/* Srctl */
	Srwrite		= 1<<29,
	Srstart		= 1<<30,
	Srdone		= 1<<31,

	/* Sr offsets */
	Ctlword		= 0x00,
	Sanadr		= 0x28,
	Vpdptr		= 0x2f,
	Altautoldptr	= 0x3e,
	Srcksum		= 0x3f,

	/* Srfla */
	Locked	= 1<<6,

	/* Atqlen, Arqlen */
	Atlenmask	= (1<<10)-1,
	Atqvfe		= 1<<28,		/* vf error set by fw */
	Atqovl		= 1<<29,		/* fw: message lost */
	Atqcrit		= 1<<30,		/* fw: critical error */
	Atqenable	= 1<<31,		/* driver: enable */

	/* Qalloc */
	Firstq		= 1<<0,
	Lastq		= 1<<15,
	Valid		= 1<<31,

	/* Txctl */
	Pfqueue		= 2,
	Pfidx		= 1<<2,

	/* Txen, Rxen */
	Enreq		= 1<<0,
	Fastdisable	= 1<<1,
	Enabled		= 1<<2,

	/* Rpadctl */
	Rpad		= 1<<0,

	/* Vsiqbase */
	Vsicontig	= 0<<11,

	/* Vsilanqtable */
	Qidx0		= 1<<0,
	Qidx1		= 1<<16,

	/* Hmsdpart0 */
	Basemsk		= (1<<12)-1,
	Segentries	= 1<<16,		/* 12 bits */

	/* Hmsaddrlo */
	Sdvalid		= 1<<0,		/* sd is valid */
	Sdpaged		= 0<<1,		/* sd is valid */
	Sdcount		= 1<<2,		/* number of sds to program: 9 bits */

	/* Hmsdcmd */
	Hmcmdwrite	= 1<<31,


	/* ctx direct programming (cheating) */
	/* Ctxctl */
	Ctxqnum	= 1<<0,
	Ctxline		= 1<<12,
	Ctxqtype		= 1<<15,		/* 0=rx, 1=tx */
	Ctxop		= 1<<17,		/* 0=read, 1=write, 2=inval */

	/* Ctxstat */
	Ctxdone		= 1<<0,
	Ctxmiss		= 1<<1,

	/* Ctxdata */
	Ctxregoff	= 0x80/4,

	/* commands */

	/* general */
	Fwver		= 1,
	Swver		= 2,
	Discodev		= 10,
	Discofn		= 11,

	/* mac address */
	Getmacs		= 0x107,
	Clearpxe		= 0x110,			/* set w[0] = 2; Eexist if already cleared */

	Getsw		= 0x200,

	/* port */
	Setportparm	= 0x203,
	Getswalloc	= 0x204,

	/* vsi */
	Addvsi		= 0x210,
	Updvsi		= 0x211,
	Getvsiparm	= 0x212,

	Setvsimc		= 0x250,
	Unsetvsimc	= 0x251,
	Addvlan		= 0x252,
	Rmvlan		= 0x253,
	Setvsipromisc	= 0x254,

	/* bandwidth allocation */
	Vsibw		= 0x408,

	/* phy/mac */
	Getphy		= 0x600,
	Setmac		= 0x603,
	Setlinkup	= 0x605,
	Linkstat		= 0x607,
	Phyreset		= 0x623,			/* not in datasheet */

	/* Setmac */
	Crcen		= (1<<2)<<16,

	/* Setlinkup */
	Anstart		= 1<<1,
	Linken		= 1<<2,

	/* Setvsipromisc */
	Upe 		= 1<<0|3<<3,		/* unicast promiscuous */
	Mpe 		= 1<<1,		/* multicast promiscuous */
	Bam 		= 1<<2,		/* broadcast accept mode */

	/* Setvsimc, Unsetvsimc */
	Perfectmatch	= 1<<0,
	Hashaddr	= 1<<1,
	Ignorevlan	= 1<<2,
	Toqueue		= 1<<3,

	/* flags */
	Adone		= 1<<0,		/* fw: mark done */
	Acmp		= 1<<1,		/* fw: mark completion */
	Aerr		= 1<<2,		/* fw: mark err */
	Avfe		= 1<<3,		/* fw: fwd from vf */
	Alb		= 1<<9,		/* driver: buffer >= 512 bytes */
	Ard		= 1<<10,		/* driver: read indirect buffer */
	Avfc		= 1<<11,		/* driver: command for vf */
	Abuf		= 1<<12,		/* driver: buffer used */
	Asi		= 1<<13,		/* driver: do not interrupt when done */
	Aei		= 1<<15,		/* driver: interrupt on error (override)  */
	Afe		= 1<<16,		/* driver: flush if previous error */

	/* Getsw bits */
	/* vis connection type */
	Vdata		= 1,
	Vdefault		= 2,
	Vcascade	= 3,

	/* switch element type */
	Vmac		= 1,
	Vpf		= 2,
	Vvf		= 3,
	Vvsi		= 19,
};

enum {
	Adsz	= 32,
};

typedef	struct	Ctlr	Ctlr;
typedef	struct	Ctlrtype	Ctlrtype;
typedef	struct	Rd	Rd;
typedef	struct	Rbpool	Rbpool;
typedef	struct	Stat	Stat;
typedef	struct	Td	Td;
typedef	struct	Ad	Ad;

enum {
	xl710,
	Nctlrtype,
};

struct Ctlrtype {
	int	type;
	int	mtu;
	int	flag;
	char	*name;
};

static Ctlrtype cttab[Nctlrtype] = {
	xl710,	9*1024+512,		0,		"i40",
};

struct Stat {
	uint	reg;
	char	*name;
};

static Stat stattab[] = {
	0x300080,	"crc error",
	0x3000e0,	"illegal byte",
	0x300020,	"mac locflt",
	0x300040,	"mac rmtflt",

	0x3000a0,	"rx length err",
	0x300140,	"xon rx",
	0x300160,	"xoff rx",
	0x300180,	"pxon rx",
	0x300280,	"pxoff rx",
	0x300480,	"rx 040",
	0x3004a0,	"rx 07f",
	0x3004c0,	"rx 100",
	0x3004e0,	"rx 200",
	0x300500,	"rx 3ff",
	0x300520,	"rx std",
	0x300540,	"rx big",
	0x300000,	"rx ok",
	0x300560,	"rx frag",
	0x300120,	"rx ovrsz",
	0x300580,	"rx jab",
	0x300620,	"loop discard",
	0x300640,	"storm discard",
	0x300660,	"rx no dest",
	0x300600,	"rx discard pkt",
	0x3005a0,	"rx ucast pkt",
	0x3005c0,	"rx mcast pkt",
	0x3005e0,	"rx bcast pkt",

	0x300980,	"xon tx",
	0x3009a0,	"xoff tx",
	0x300780 ,	"pxon tx",
	0x300880,	"pxoff tx",
	0x3006a0,	"tx 040",
	0x3006c0,	"tx 07f",
	0x3006e0,	"tx 100",
	0x300700,	"tx 200",
	0x300720,	"tx 3ff",
	0x300740,	"tx 3ff",
	0x300760,	"tx big",
	0x300a20,	"tx nolink",
	0x3009c0,	"tx ucast pkt",
	0x3009e0,	"tx mcast pkt",			/* swap with bcast? */
	0x300a00,	"tx bcast pkt",
	0x300680,	"tx byte",

	0x370180,	"sw rx unk",
	0x35c000,	"sw rx byte",
	0x370000,	"sw rx ucast pkt",
	0x370080,	"sw rx mcast pkt",
	0x370100,	"sw rx bcast pkt",
	
	0x348000,	"sw tx discard",
	0x32c000,	"sw tx byte",
	0x340000,	"sw tx ucast pkt",
	0x340080,	"sw tx mcast pkt",
	0x340100,	"sw tx bcast pkt",
};

static Stat vstattab[] = {
	0x358000,	"vsi rxgd",
	0x358000,	"vsi rx ucast pkt",
	0x36cc00,	"vsi rx mcast pkt",
	0x36d800,	"vsi rx bcast pkt",
	0x310000,	"vsi rx discard",
	0x36e400,	"vsi rx unk",
	0x328000,	"vsi txgd",
	0x328004,	"vsi txgd byte",
	0x33c000,	"vsi tx ucast pkt",
	0x33cc00,	"vsi tx mcast pkt",
	0x33d800,	"vsi tx bcast pkt",
	0x344000,	"vsi tx err pkt",
};

#define	parm0	w[0]
#define	parm1	w[1]
#define	adrhi	w[2]
#define	adrlo	w[3]

struct Ad {
	u16int	flag;
	u16int	op;
	u16int	len;
	u16int	rv;
	uchar	cookie[8];
	u32int	w[4];
};

/* status */
enum {
	l3l4p	= 1<<3,	/* ip: l3 and l4 checksum */
	L2tag	= 1<<2,	/* l2 tag strip */
	Reop	= 1<<1,	/* end of packet */
	Rdd	= 1<<0,	/* descriptor done */
};

struct Rd {
	u64int	addr;
	u32int	status;		/* must be zero on tx */
	u32int	length;		/* >>6 */
};

enum{
	/* Td cmd */
	Il2tag	= 1<<7,	/* insert l2 tag; 8100 (vlan) by default */
	Mb1	= 1<<6,	/* must be 1 */
	Rs	= 1<<5,
	Teop	= 1<<4,
	Txdesc	= 0<<0,
	Nopdesc	= 1<<0,

	/* Td status */
	Tdd	= 0xf<<0,
};

struct Td {
	u64int	addr;
	u16int	cmd;
	u16int	offset;
	u16int	length;
	u16int	tag1;
};

enum{
	Factive	= 1<<0,
	Fstarted	= 1<<1,
};

typedef void (*Freefn)(Block*);
typedef u64int	FPDE;

enum {
	Hmcpgsz		= 4096,
	Hmcalign	= 4096,
	Fpdeperpage	= Hmcpgsz/sizeof(FPDE),
	Fpgshift		= 12,

	Fpdevalid	= 1<<0,
};

enum {
	Cnta,
	Cntr,
	Cntt,
	Cnts,		/* spurious */
};

struct Ctlr {
	Pcidev	*p;
	uintmem	port;
	uint	pf, npf, vsiseid, qsh;
	uint	tqno, rqno;
	u32int	*reg;
	uchar	flag;
	FPDE	*pdpage;
	void	*pagetab[1];
	uint	poolno;
	Rbpool	*pool;
	Ad	*atq, *arq;
	uint	ath, att, arh, art;
	int	nrd, ntd, nrb, rbsz;
	Lock	alock;
	QLock	tlock;
	Rendez	arendez, trendez, rrendez;
	uint	im, aim, rim, tim;
	Lock	imlock;
	char	*alloc;
	Rd	*rdba;
	Block	**rb;
	uint	rdt, rdfree;
	Td	*tdba;
	uint	tdh, tdt, tdfree;
	Block	**tb;
	uchar	ra[Eaddrlen];
	uvlong	zstats[nelem(stattab)];
	uvlong	zvstats[nelem(vstattab)];
	uchar	*rxbuf;
	int	type;
	uint	speeds[5];
	uint	phy;
	uint	nobufs, txfull;
	uvlong	irqcnt[4];
	char	ename[16];
};

struct Rbpool {
	union {
		struct {
			Lock;
			Block	*b;
			uint	nstarve;
			uint	nwakey;
			uint	starve;
			Rendez;
		};
		uchar pad[64];		/* cacheline */
	};
	union {
		struct {
			Block	*x;
			uvlong	fasttick;
			uvlong	slowtick;
			uint	nfast;
			uint	nslow;
		};
		uchar pad[64];		/* cacheline */
	};
};

/* tweakable parameters */
enum{
	Nrd	= 256,		/* 2ⁿ */
	Ntd	= 2*256,		/* 2ⁿ */
	Nrb	= 2048,
	Nctlr	= 8,
	Rbalign	= 8,
	Natq	= 8,
	Narq	= 8,
	Arbufsz	= 64,
};

static	Ctlr	*ctlrtab[Nctlr];
static	Lock	rblock[Nctlr];
static	Rbpool	rbtab[Nctlr];
static	int	nctlr;

static char* errtab[] = {
	"ok",
	"Operation not permitted",
	"No such element",
	"Bad opcode",
	"operation interrupted",
	"I/O error",
	"No such resource",
	"Arg too long",
	"Try again",
	"Out of memory",
	"Permission denied",
	"Bad address",
	"Device or resource busy",
	"Attempt to crate something that exists",
	"Invalid argument",
	"Not a typewriter",				/* humor?  one hopes. */
	"No space left or allocation failure",
	"Function not implemented",
	"Parameter out of range",
	"Command flushed because a previous command completed in error",
	"nternal error, descriptor contains a bad pointer",
	"Operation not allowed in current device mode",
	"File Too Big",
};
static uint speeds[] = {0, 100, 1000, 10000, 40000, };

enum {
	Dcmd	= 1<<0,
	Dvsi	= 1<<1,
	Dswitch	= 1<<2,
	Debug	= 0,
};
#define dprint(f, ...)	do{if(f&Debug)print(__VA_ARGS__);}while(0)

#define	getseid(q)	(uint)getle(q, 2)

static char*
cname(Ctlr *c)
{
	return cttab[c->type].name;
}

static char*
ename(Ether *e)
{
	Ctlr *c;

	c = e->ctlr;
	if(c->ename[0] == 0)
		snprint(c->ename, sizeof c->ename, "#l%d: %s:pf %d",
			e->ctlrno, cname(c), c->pf);
	return c->ename;
}

static void
readclear(u32int)
{
}

static int
seidtovsiid(int i)
{
	return i-512;
}

static void
readstats(Ctlr *c, uvlong *stats)
{
	int i, o;

	o = c->pf;
	for(i = 0; i < nelem(c->zstats); i++)
		stats[i] = c->reg[8*o+stattab[i].reg>>2];
}

static void
readvstats(Ctlr *c, uvlong *stats)
{
	int i, o;

	o = seidtovsiid(c->vsiseid);
	for(i = 0; i < nelem(c->zvstats); i++)
		stats[i] = c->reg[8*o+vstattab[i].reg>>2];
}

static long
ifstat(Ether *e, void *a, long n, usize offset)
{
	char *s, *p, *q;
	uint i;
	uvlong v, *stats;
	Ctlr *c;
	Rbpool *r;

	c = e->ctlr;
	p = s = malloc(READSTR + nelem(stattab)*sizeof(uvlong));
	if(p == nil)
		error(Enomem);
	q = p+READSTR;
	stats = (uvlong*)q;

	readstats(c, stats);
	for(i = 0; i<nelem(stattab); i++)
		if((v = stats[i] - c->zstats[i])>0)
			p = seprint(p, q, "%.10s	%llud\n", stattab[i].name, v);
	readvstats(c, stats);
	for(i = 0; i<nelem(vstattab); i++)
		if((v = stats[i] - c->zvstats[i])>0)
			p = seprint(p, q, "%.10s	%llud\n", vstattab[i].name, v);
	p = seprint(p, q, "type: %s\n", cttab[c->type].name);
	r = c->pool;
	p = seprint(p, q, "pool: fast %d %llud slow %d %lld starve %d wake %d on %d\n",
		r->nfast, r->fasttick,
		r->nslow, r->slowtick,
		r->nstarve, r->nwakey, r->starve);
	p = seprint(p, q, "speeds:");
	for(i = 0; i < nelem(c->speeds); i++)
		p = seprint(p, q, " %d:%d", speeds[i], c->speeds[i]);
	p = seprint(p, q, "\n");
	p = seprint(p, q, "phy: %.2ux\n", c->phy);
	p = seprint(p, q, "irqcnt: a %lld r %lld t %lld s %lld\n",
		c->irqcnt[Cnta], c->irqcnt[Cntr], c->irqcnt[Cntt], c->irqcnt[Cnts]);
	seprint(p, q, "nobufs: %ud\n", c->nobufs);
	seprint(p, q, "txfull: %ud\n", c->txfull);
	n = readstr(offset, a, n, s);
	free(s);

	return n;
}

static void
adinitrx(Ctlr *c, int i)
{
	Ad *a;

	a = c->atq + i;
	memset(a, 0, Adsz);
	a->flag = Abuf;
	a->len = Arbufsz;
	a->adrhi = Pciwaddrh(c->rxbuf + Arbufsz*i);
	a->adrlo = Pciwaddrl(c->rxbuf + Arbufsz*i);
}

static void
adinitpoll(Ad *cmd)
{
	memset(cmd, 0, Adsz);
	cmd->flag = Asi;			/* don't generate another irq */
}

static int
aqsend(Ctlr *c, Ad *cmd)
{
	Ad *a;
	static Lock l;			/* not good enough; ring wrap? */

	ilock(&l);
	a = c->atq + c->att;
	memmove(a, cmd, Adsz);
	sfence();
	c->att = NEXT(c->att, Natq);
	c->reg[Atqt + c->pf] = c->att;
	iunlock(&l);

	while(c->reg[Atqh + c->pf] != c->att)
		microdelay(1);
	memmove(cmd, a, Adsz);
	if(cmd->rv != 0 && cmd->rv < nelem(errtab))
		print("%s: cmd %#x done rv=%s\n", cname(c), cmd->op, errtab[cmd->rv]);
	else
		dprint(Dcmd, "%s: cmd %#x done rv=%d\n", cname(c), cmd->op, cmd->rv);
	return cmd->rv;
}

static int
linkrate(Ether *e, Ctlr *c, Ad *a)
{
	uchar *b, link, rate;
	int i;

	b = (uchar*)a->w + 2;
	link = b[2] & 1;
	rate = b[1];

	if(link == 0)
		rate = 0;
	i = log2ceil(rate & 0x1f);
	e->link = link;
	e->mbps = speeds[i];
	c->speeds[i]++;
	c->phy = b[0];

//	print("link %d; mbps %d; an %d; phy %.2ux\n", e->link, e->mbps, b[3] & 1<<0, b[0]);
	return 0;
}

static void
im(Ctlr *c, int i)
{
	ilock(&c->imlock);
	c->im |= i;
	c->reg[Icren + c->pf] = c->im;
	iunlock(&c->imlock);
}

static void
txim(Ctlr *c, int on)
{
	c->reg[Itxqctl + 0] = Itxidx*Iqirqidx | Iqend | on*Iqenable;			/* tx: ics=0x02 */
}

static void
rxim(Ctlr *c, int on)
{
	c->reg[Irxqctl + 0] = Irxidx*Iqirqidx | 0*Iqnxtqidx | Iqnxtt | on*Iqenable;	/* tx: ics=0x04 */
}

static void
qrx(Ether *e, Ctlr *c, Ad *a)
{
	switch(a->op){
	case Linkstat:
		USED(e, c, a);
//		linkrate(e, c, a);
		break;
	default:
		print("%s: rxcmd %#ux unknown\n", cname(c), a->op);
		break;
	}
}

static int
aim(void *v)
{
	return ((Ctlr*)v)->aim != 0;
}

static void
aproc(void *v)
{
	uint h, end, oldaim, pf;
	Ad cmd, *a;
	Ctlr *c;
	Ether *e;

	e = v;
	c = e->ctlr;
	pf = c->pf;
	im(c, Adminq|Linkchange);
	oldaim = Linkchange;
loop:
	c->aim = 0;

	if(oldaim & (Eccerr | Malice | Grst | Pcierr)){
		print("%s: err %.8ux; we're doomed\n", ename(e), oldaim & Errint);
	}
	if(oldaim & Errint){
		/*
		 * 0:4	pf
		 * 8:11	error
		 *	0	private memory fn invalid
		 *	1	invalid lan queue idx
		 *	2	object index larger than glhmc_*cnt
		 *	3	address beyond pf's segment limits
		 *	4	invalid segment descriptor
		 *	5	sd too small
		 *	6	page descriptor invalid
		 *	7	unsupported pcie read
		 *	8	pflan_qalloc_pmat or pf_vt_pfalloc_pmat unset
		 *	9	invalid object type
		 *	10	invalid fcoe idx
		 * 16:20		object type	(tx=0, rx=1)
		 * 31		error detected
		 */
		print("%s: hmcerr %.8ux %.8ux\n", ename(e),
			c->reg[Hmerr+c->pf], c->reg[Hmerrd+c->pf]);
	}
	/*
	 * fixme: it's not necessary to process link status changes both
	 * for Adminq commands, and for Linkchange irqs.  currently
	 * this is left in because we'd like to verify that we get adminq
	 * commands, and this is the only one currently observed.
	 */
	if(oldaim & Adminq){
		end = c->reg[Arqh+ pf];
		for(h = c->arh; h != end; h = NEXT(h, Narq)){
			a = c->arq + h;
			print("%s: cmd slot %d op %#ux\n", ename(e), h, a->op);
			qrx(e, c, a);
			adinitrx(c, h);
			c->reg[Arqt + pf] = h;
		}
		c->arh = h;
	}
	if(oldaim & Linkchange){
		adinitpoll(&cmd);
		cmd.op = Linkstat;
	//	cmd.parm0 = 3;			/* link status enable */
		aqsend(c, &cmd);
		linkrate(e, c, &cmd);
	}

	im(c, Aprocint);
	sleep(&c->arendez, aim, c);
	oldaim = c->aim;
	goto loop;
}

static long
ctl(Ether *, void *, long)
{
	error(Ebadarg);
	return -1;
}

static int
icansleep(void *v)
{
	Rbpool *p;
	int r;

	p = v;
	ilock(p);
	r = p->starve == 0;
	iunlock(p);

	return r;
}

static Block*
rballoc(Rbpool *p)
{
	Block *b;
	uvlong t;

	t = -rdtsc();
	for(;;){
		if((b = p->x) != nil){
			p->nfast++;
			p->x = b->next;
			b->next = nil;
			p->fasttick += t+rdtsc();
			return b;
		}

		ilock(p);
		b = p->b;
		p->b = nil;
		if(b == nil){
			p->starve = 1;
			p->nstarve++;
			iunlock(p);
			return nil;
		}
		p->nslow++;
		p->slowtick += t+rdtsc();
		iunlock(p);
		p->x = b;
	}
}

static void
rbfree(Block *b, int t)
{
	Rbpool *p;

	p = rbtab + t;
	b->rp = b->wp = (uchar*)ROUNDUP((uintptr)b->base, Rbalign);
	b->flag &= ~(Bipck | Budpck | Btcpck | Bpktck);

	ilock(p);
	b->next = p->b;
	p->b = b;
	if(p->starve){
		if(1)
			iprint("wakey %d; %d %d\n", t, p->nstarve, p->nwakey);
		p->nwakey++;
		p->starve = 0;
		iunlock(p);
		wakeup(p);
	}else
		iunlock(p);
}

static void
rbfree0(Block *b)
{
	rbfree(b, 0);
}

static void
rbfree1(Block *b)
{
	rbfree(b, 1);
}

static void
rbfree2(Block *b)
{
	rbfree(b, 2);
}

static void
rbfree3(Block *b)
{
	rbfree(b, 3);
}

static void
rbfree4(Block *b)
{
	rbfree(b, 4);
}

static void
rbfree5(Block *b)
{
	rbfree(b, 5);
}

static void
rbfree6(Block *b)
{
	rbfree(b, 6);
}

static void
rbfree7(Block *b)
{
	rbfree(b, 7);
}

static Freefn freetab[Nctlr] = {
	rbfree0,
	rbfree1,
	rbfree2,
	rbfree3,
	rbfree4,
	rbfree5,
	rbfree6,
	rbfree7,
};

#define Next(x, m)	(((x)+1) & (m))
static int
cleanup(Ctlr *c, int tdh)
{
	Block *b;
	uint m, n;

	m = c->ntd-1;
	while((c->tdba[n = Next(tdh, m)].cmd&Tdd) == Tdd){
		tdh = n;
		b = c->tb[tdh];
		c->tb[tdh] = 0;
		freeb(b);
		c->tdba[tdh].cmd = 0;
	}
	return tdh;
}

static void
transmit(Ether *e)
{
	uint i, m, tdt, tdh;
	Ctlr *c;
	Block *b;
	Td *t;

	c = e->ctlr;
//	qlock(&c->tlock);
	if(!canqlock(&c->tlock)){
		im(c, Itx);
		return;
	}
	tdh = c->tdh = cleanup(c, c->tdh);
	tdt = c->tdt;
	m = c->ntd-1;
	for(i = 0; i<16; i++){
		if(Next(tdt, m) == tdh){
			im(c, Itx);
			break;
		}
		if(!(b = qget(e->oq)))
			break;
		t = c->tdba+tdt;
		t->addr = Pciwaddr(b->rp);
		t->length = BLEN(b)<<2;
		t->offset = (6+6+2)/2;	// cksum offload # header words up to etype
		t->cmd = Rs|Mb1|Teop;
		c->tb[tdt] = b;
		tdt = Next(tdt, m);
	}
	if(i){
		c->tdt = tdt;
		sfence();
		c->reg[Txtail + c->tqno] = tdt;
	}
	qunlock(&c->tlock);
}

static int
tim(void *c)
{
	return ((Ctlr*)c)->tim != 0;
}

static int
·cleanup(Ctlr *c, int tdh)
{
	Block *b;
	uint m, n;

	m = c->ntd-1;
	while(c->nrd - c->tdfree > 16 && (c->tdba[n = Next(tdh, m)].cmd&Tdd) == Tdd){
		tdh = n;
		b = c->tb[tdh];
		c->tb[tdh] = nil;
		freeb(b);
	//	c->tdba[tdh].cmd = 0;
		c->tdfree++;
	}
	return tdh;
}

static void
tproc(void *v)
{
	uint m, w;
	Block *b;
	Ctlr *c;
	Ether *e;
	Td *t;

	e = v;
	c = e->ctlr;
	m = c->ntd-1;
	w = 0;
	for(;;){
		c->tdh = ·cleanup(c, c->tdh);
		if(Next(c->tdt, m) == c->tdh){
			c->txfull++;
/**/			print("%s: tproc full\n", ename(e));
			txim(c, 1);
			sleep(&c->trendez, tim, c);
			txim(c, 0);
/**/			print("%s: tproc tim %.8ux\n", ename(e), c->tim);
			c->tim = 0;
			continue;
		}
		if(w > 8 || qlen(e->oq) == 0){
			c->reg[Txtail + c->tqno] = c->tdt;
			w = 0;
		}
		b = qbread(e->oq, 100000);
		t = c->tdba+c->tdt;
		t->addr = Pciwaddr(b->rp);
		t->length = BLEN(b)<<2;
		t->offset = (6+6+2)/2;	// cksum offload # header words up to etype
		t->cmd = Rs|Mb1|Teop|Txdesc;
		sfence();
		c->tb[c->tdt] = b;
		c->tdt = Next(c->tdt, m);
		c->reg[Txtail + c->tqno] = c->tdt;
		c->tdfree--;
		w++;
	}
}

static int
replenish(Ctlr *c, uint rdh, int maysleep)
{
	int rdt, m, i;
	Block *b;
	Rd *r;
	Rbpool *p;

	m = c->nrd-1;
	i = 0;
	p = c->pool;
	for(rdt = c->rdt; c->nrd-c->rdfree >= 16 && Next(rdt, m) != rdh; rdt = Next(rdt, m)){
		r = c->rdba+rdt;
		while((b = rballoc(c->pool)) == nil){
			c->nobufs++;
			if(maysleep == 0)
				goto nobufs;
			print("%s:%d: starve\n", cname(c), c->poolno);
			sleep(p, icansleep, p);
		}
		c->rb[rdt] = b;
		r->addr = Pciwaddr(b->rp);
		r->status = 0;
		r->length = 0;
		c->rdfree++;
		i++;
	}
nobufs:
	if(i){
		sfence();
		c->reg[Rxtail + c->rqno] = c->rdt = rdt;
	}
	if(rdt == rdh)
		return -1;
	return 0;
}

static int
rim(void *v)
{
	return ((Ctlr*)v)->rim != 0;
}

static void
rproc(void *v)
{
	Ether *e;
	Ctlr *c;
	Block *b;
	Rd *r;
	uint m, rdh;

	e = v;
	c = e->ctlr;
	m = c->nrd-1;
	rdh = 0;
	im(c, Irx);
loop:
	replenish(c, rdh, 1);
	rxim(c, 1);
im(c, Irx);	/* this isn't right; still big delays. */
	sleep(&c->rrendez, rim, c);
//	rxim(c, 0);
loop1:
	c->rim = 0;
	if(c->nrd-c->rdfree >= 24)
	if(replenish(c, rdh, 0) == -1)
		goto loop;
	r = c->rdba+rdh;
	if(!(r->status&Rdd))
		goto loop;
	b = c->rb[rdh];
	c->rb[rdh] = nil;
	b->wp += r->length>>6;
	b->lim = b->wp;		/* lie like a dog */
	if(r->status&l3l4p)
		b->flag |= Bipck|Btcpck|Budpck;
//	r->status = 0;
	etheriq(e, b, 1);
	c->rdfree--;
	rdh = Next(rdh, m);
	goto loop1;
}

static void
setpromisc(Ctlr *c, int flags, int vsi)
{
	Ad cmd;

	adinitpoll(&cmd);
	cmd.op = Setvsipromisc;
	cmd.w[0] = flags | flags<<16;
	cmd.w[1] = 1<<15 | vsi;
	aqsend(c, &cmd);
}

static void
promiscuous(void *a, int on)
{
	Ether *e;
	Ctlr *c;

	e = a;
	c = e->ctlr;
	if(on)
		setpromisc(c, Upe|Mpe|Bam, c->vsiseid);
	else
		setpromisc(c, Bam, c->vsiseid);
}

/*
 * this is the only use of the admin queue outside aproc.  hopefully
 * the necessary lock will not be an issue.
 */
static int
setmc(Ctlr *c, int on, int vsi, uchar *ea)
{
	uchar buf[16];
	Ad cmd;

	adinitpoll(&cmd);
	cmd.op = on? Setvsimc: Unsetvsimc;
	cmd.flag |= Abuf | Ard;
	cmd.len = sizeof buf;
	cmd.w[0] = (1<<15 | vsi)<<16 | 1;
	cmd.adrlo = Pciwaddrl(buf);
	cmd.adrhi = Pciwaddrh(buf);

	memset(buf, 0, sizeof buf);
	memmove(buf, ea, 6);
	putle(buf+6, 0, 2);		/* vlan tag */
	putle(buf+8, Perfectmatch, 2);	/* flags */
	putle(buf+10, 0, 2);		/* q#; valid if Toqueue set */

	return aqsend(c, &cmd);
}

static void
multicast(void *a, uchar *ea, int on)
{
	char buf[64];
	uint rv;
	Ether *e;
	Ctlr *c;

	e = a;
	c = e->ctlr;
	rv = setmc(c, on, c->vsiseid, ea);
	if(rv != 0){
		snprint(buf, sizeof buf, "%s: multicast: error %d", ename(e), rv);
		if(rv < nelem(errtab))
			snprint(buf, sizeof buf, "%s: multicast: %s", ename(e), errtab[rv]);
		error(buf);
	}
}

static int
detach(Ctlr *c)
{
	int i, wait;

	wait = (c->reg[Rstctl] & 0x3f)*100;
	for(i = 0;; i++){
		if(i == wait)
			return -1;
		if((c->reg[Rststat] & Devstate) == 0)
			break;
		delay(1);
	}
	if(i == 0){
		/* need to use pf here not the port# */
		c->reg[Ctl] |= Sreset;
		for (i = 0; i < 10; i++) {
			if(i == 10)
				return -1;
			if((c->reg[Ctl] & Sreset) == 0)
				break;
			delay(1);
		}
	}
	return 0;
}

static void
shutdown(Ether *e)
{
	detach(e->ctlr);
}

static void
allocmem(Ctlr *c)
{
	int t;

	c->nrd = Nrd;
	c->ntd = Ntd;
	t = c->nrd*sizeof *c->rdba+255;
	t += c->ntd*sizeof *c->tdba+255;
	t += (c->ntd+c->nrd)*sizeof(Block*);
	t += Natq*Adsz+64;
	t += Narq*Adsz+64;
	t += Narq * Arbufsz;
	c->alloc = malloc(t);
	if(c->alloc == nil)
		panic("%s: no memory", cname(c));

	c->rdba = (Rd*)ROUNDUP((uintptr)c->alloc, 256);
	c->tdba = (Td*)ROUNDUP((uintptr)(c->rdba+c->nrd), 256);
	c->rb = (Block**)(c->tdba+c->ntd);
	c->tb = (Block**)(c->rb+c->nrd);
	c->atq = (Ad*)ROUNDUP((uintptr)(c->tb+c->ntd), 64);
	c->arq = (Ad*)ROUNDUP((uintptr)(c->atq+Natq), 64);
	c->rxbuf = (uchar*)(c->arq+Narq);
}

static void
irqdisable(Ctlr *c)
{
	int pf;

	for(pf = 0; pf < c->npf; pf++){
		c->reg[Icren + pf] = 0;
		c->reg[Ilnklst + pf] = 0;
		c->reg[Idynctl + pf] = 0;
		c->reg[Icr + pf] = 0;
	}
}

void
aqinit(Ctlr *c)
{
	int pf;
	u32int *r;
	Ad cmd;

	pf = c->pf;

	/* driver → firmware */
	c->ath = 0;
	c->att = 0;
	c->reg[Atqh + pf] = c->ath;
	c->reg[Atqt + pf] = c->att;
	c->reg[Atqlo + pf] = Pciwaddrl(c->atq);
	c->reg[Atqhi + pf] = Pciwaddrh(c->atq);

	/* fw → driver */
	c->arh = 0;
	c->art = 0;
	c->reg[Arqh + pf] = c->arh;
	c->reg[Arqt + pf] = c->art;
	c->reg[Arqlo + pf] = Pciwaddrl(c->arq);
	c->reg[Arqhi + pf] = Pciwaddrh(c->arq);

	c->reg[Atqlen + pf] = Natq | Atqenable;
	c->reg[Arqlen + pf] = Narq | Atqenable;

	for(int j = 0; j < Narq; j++){
		adinitrx(c, j);
	}
	c->art = Narq-1;
	c->reg[Arqt + pf] = c->art;

	/* must send getversion command */
	adinitpoll(&cmd);
	cmd.op = Fwver;
	aqsend(c, &cmd);

	/* 16 - 19 rom version */
	/* 20 - 23 fw build */
	/* 24 - 27 fw version */
	/* 27 - 31 queue version */

	r = (u32int*)&cmd;
	if(r[7]>>16 > 1)
		error("unsupported fw version");

	adinitpoll(&cmd);
	cmd.op = Swver;
	aqsend(c, &cmd);
}

static void
aqmac(Ctlr *c, uchar *ea)
{
	Ad cmd;
	uchar macs[4][6];
	enum {Lan, San, Port, Wol};

	adinitpoll(&cmd);
	cmd.op = Getmacs;
	cmd.flag |= Abuf;
	cmd.len = sizeof(macs);
	cmd.adrlo = Pciwaddrl(macs);
	cmd.adrhi = Pciwaddrh(macs);
	aqsend(c, &cmd);
	memmove(ea, macs[Lan], 6);
}

static int
reset(Ctlr *c)
{
	if(detach(c)){
		print("%s: reset timeout\n", cname(c));
		return -1;
	}
	allocmem(c);
	delay(16);		/* why? otherwise we hang: issue coraid-04 */
	aqinit(c);
	aqmac(c, c->ra);
	return 0;
}

void
linkinit(Ctlr *c)
{
	Ad cmd;

	adinitpoll(&cmd);
	cmd.op = Setmac;
	cmd.w[0] = cttab[c->type].mtu | Crcen;
	aqsend(c, &cmd);

	adinitpoll(&cmd);
	cmd.op = Setlinkup;
	cmd.w[0] = Anstart;		//Linken | Anstart;
	aqsend(c, &cmd);

	/* disable pause here */
}

#define Pageno(fpa)	((fpa)>>Fpgshift)
#define Pfpageidx(pf, fpa)	(Pageno(fpa) - Pageno(2*MiB*(pf)))
#define Hmcblock(fpa)	((fpa)>>9)

static void
hmcmap(Ctlr *c, u64int fpa)
{
	uint pfpg;

	if(fpa > Fpdeperpage * Hmcpgsz)
		panic("%s: hmcmap:  %#llux\n", cname(c), fpa);
	if(fpa & (1<<Fpgshift)-1)
		panic("%s: hmcmap: misaligned fpa %llux", cname(c), fpa);
	pfpg = Pfpageidx(c->pf, fpa);
	if(pfpg >= nelem(c->pagetab))
		panic("%s: hmcmap", cname(c));
if(c->pf==1)print("PFPG	%d\n", pfpg);
	if(c->pagetab[pfpg] != nil)
		return;
	c->pagetab[pfpg] = mallocalign(Hmcpgsz, Hmcalign, 0, 0);
}

static void
hmcfault(Ctlr *c, u64int fpa)
{
	uint pfpg;

	hmcmap(c, fpa);
	pfpg = Pfpageidx(c->pf, fpa);
	c->pdpage[pfpg] = Pciwaddr(c->pagetab[pfpg]) | Fpdevalid;
}

static void
hmcinit(Ctlr *c)
{
	uint pf;

	pf = c->pf;
	c->pdpage = mallocalign(Hmcpgsz, Hmcalign, 0, 0);

	/*
	 * allocate 2M of card's internal virtual address space
	 * to each pf.   populate each card's addres space with
	 * 1 FPDE at [2M*pf, 2M*pf+(1<<Ppgshift)).
	 */
	c->reg[Hmsdpart + pf] = Pageno(pf*2*MiB) | 1*Segentries;

	/*
	 * allocate 4 rx and 4 tx queues.  allocation is in 512b blocks.
	 * divide our single allocated page in half between tx/rx.
	 */
	c->reg[Hmtxbase + pf] = (0+2*MiB*pf)/512;
	c->reg[Hmtxcnt + pf] = 4;
	c->reg[Hmrxbase + pf] = (0+2*MiB*pf + Hmcpgsz/2)/512;
	c->reg[Hmrxcnt + pf] = 4;

	/*
	 * fault lowest pf virtual address
	 */
	hmcfault(c, 0+2*MiB*pf);

	/*
	 * program SD with our pdpage.
	 */
	c->reg[Hmsdaddrhi + pf] = Pciwaddrh(c->pdpage);
	c->reg[Hmsdaddrlo + pf] = Pciwaddrl(c->pdpage) | 1*Sdcount | Sdvalid | Sdpaged;
	c->reg[Hmsdcmd + pf] = Hmcmdwrite | Pageno(pf*2*MiB);
}

static void
swdump(uchar *u)
{
	char *el, *conn, cbuf[16];
	uchar *p;
	int i, n;

	if((Debug & Dswitch) == 0){
		USED(u);
		return;
	}

	n = getle(u, 2);
	print("%d switch elements of %d\n", n, (uint)getle(u+2, 2));
	for(i = 0; i < n; i++){
		p = u + 16*(i+1);

		switch(p[11]){
		case Vdata:
			conn = "data";
			break;
		case Vdefault:
			conn = "default";
			break;
		case Vcascade:
			conn = "cascade";
			break;
		default:
			snprint(cbuf, sizeof cbuf, "res/%d", p[11]);
			conn = cbuf;
			break;
		}
		switch(p[0]){
		case Vmac:
			el = "mac";
			break;
		case Vpf:
			el = "pf";
			break;
		case Vvf:
			el = "vf";
			break;
		case Vvsi:
			el = "vsi";
			break;
		default:
			el = "??";
			break;
		}
		print("%d (seid)	up=%d dn=%d	%s %s id=%d\n", getseid(p+2), 
			getseid(p+4), getseid(p+6), conn, el, (uint)getle(p+14, 2));
	}
}

static void
swproc(Ctlr *c, uchar *u)
{
	uchar *p;
	int i, n;

	n = getle(u, 2);
	for(i = 0; i < n; i++){
		p = u + 16*(i+1);

		switch(p[0]){
		case Vvsi:
			if(c->vsiseid == -1)
			if(p[11] == Vdata)
				c->vsiseid = getseid(p+2);
			break;
		}
	}
}

static void
vsidump(uchar *u)
{
	uint v, i;

	USED(u);
	if((Debug & Dvsi) == 0)
		return;

	print("valid	%ux\n", (uint)getle(u, 2));
	print("switch	%d (s-tag=%d)\n",
		(uint)getle(u+2, 2) & 0xfff, (u[3]&0x10) == 0);
	print("vlan %.5lH\n", u+8);
	print("mapmeth	%ux\n", u[28]&1);
	for(i = 0; i < 1; i++){
		v = getle(u+30+2*i, 2);
		print("q%d	%ux\n", i, v);
	}
	print("sched	%ux\n", (uint)getle(u+82, 2));
	print("qshand0	%ux\n", (uint)getle(u+96, 2));
	print("vsi stats %.4ux\n", (uint)getle(u+112, 2));
	print("schedid %.4ux\n", (uint)getle(u+114, 2));
}

static void
vsiinit(Ctlr *c)
{
	uchar buf[0x80];
	int i;
	Ad cmd;

	/* dump switch configuration and find vsi */
	c->vsiseid = -1;
	for(i = 0;;){
		memset(buf, 0, sizeof buf);
		adinitpoll(&cmd);
		cmd.op = Getsw;
		cmd.flag |= Abuf;
		cmd.len = sizeof(buf);
		cmd.adrlo = Pciwaddrl(buf);
		cmd.adrhi = Pciwaddrh(buf);
		cmd.w[0] = i;
		if(aqsend(c, &cmd) != 0)
			break;
		swdump(buf);
		swproc(c, buf);
		i = cmd.w[0] & 0xffff;
		if(i == 0)
			break;
	}

	if(c->vsiseid == -1){
		print("%s: no vsi found; we're doomed\n", cname(c));
		c->vsiseid = 515;
	}

	dprint(Dvsi, "vsiprobe %d\n", c->vsiseid);
	memset(buf, 0, sizeof buf);
	adinitpoll(&cmd);
	cmd.op = Getvsiparm;
	cmd.flag |= Abuf;
	cmd.len = sizeof(buf);
	cmd.adrlo = Pciwaddrl(buf);
	cmd.adrhi = Pciwaddrh(buf);
	cmd.w[0] = c->vsiseid;
	if(aqsend(c, &cmd) == 0)
		vsidump(buf);
}

/* §7.7.5.6.1.4; grab qsh (qset handle) */
void
vsibw(Ctlr *c)
{
	uchar buf[0x40];
	int /*i,*/ valid, susp;
	Ad cmd;

	memset(buf, 0, sizeof buf);
	adinitpoll(&cmd);
	cmd.op = Vsibw;
	cmd.flag |= Abuf;
	cmd.len = sizeof(buf);
	cmd.adrlo = Pciwaddrl(buf);
	cmd.adrhi = Pciwaddrh(buf);
	cmd.w[0] = c->vsiseid;
	if(aqsend(c, &cmd) != 0){
		print("vsibw fails\n");
		return;
	}
	valid = buf[0] & 0x7f;
	susp = buf[1] & 0x7f;

	if((valid & 1<<0) == 0 || (susp & 1<<0) != 0)
		print("%s: tc0/qs0 not enabled; we're doomed\n", cname(c));
	else
		c->qsh = getle(buf+16, 2);
//	print("tc0 on qset %#.4ux\n", c->qsh);
}

static void
irqenable(Ctlr *c)
{
	int pf;

	readclear(c->reg[Icr + c->pf]);
	for(pf = 0; pf < c->npf; pf++){
		rxim(c, 0);
		txim(c, 0);
		c->reg[Ilnklst + pf] = 0*Firstqidx | Firstrx;				/* 1st: rxq idx 0 */
		c->reg[Icren + pf] = c->im;
		c->reg[Idynctl + pf] = 0<<5 | 0<<3 | Intena | Clearpba;		/* 0*4µs coalescing */
	}
	im(c, Errint);
}

static void
allocblocks(Ctlr *c)
{
	Block *b;

	for(c->nrb = 0; c->nrb < Nrb; c->nrb++){
		b = allocb(c->rbsz+Rbalign);
		b->free = freetab[c->poolno];
		freeb(b);
	}
}

static void
txcontext(Ctlr *c, uchar *u)
{
	uintmem p;

	memset(u, 0, 128);
	u[3] |= 1<<6;			/* new context */
	p = Pciwaddr(c->tdba)/128;
	u[4] |= p;
	u[5] |= p>>8;
	u[6] |= p>>16;
	u[7] |= p>>24;
	u[8] |= p>>32;
	u[9] |= p>>40;
	u[10] |= p>>48;
	u[11] |= p>>56;
	p = c->ntd<<1;
	u[20] |= p;
	u[21] |= p>>8;
	p = 7<<6;
	u[21] |= p;
	u[22] |= p>>8;
	p = c->qsh<<4;		/* qshand0 from vsi */
	u[122] |= p;
	u[123] |= p>>8;
}

/* § 8.3.3.2.2 */
static void
rxcontext(Ctlr *c, uchar *u)
{
	uintmem p;

	memset(u, 0, 32);
	p = Pciwaddr(c->rdba)/128;
	u[4] |= p;
	u[5] |= p>>8;
	u[6] |= p>>16;
	u[7] |= p>>24;
	u[8] |= p>>32;
	u[9] |= p>>40;
	u[10] |= p>>48;
	u[11] |= p>>56;
	p = c->nrd<<1;
	u[11] |= p;
	u[12] |= p>>8;
	p = c->rbsz/128<<6;
	u[12] |= p;
	u[13] |= p>>8;
	u[14] |= 1<<5;
	p = c->rbsz+4<<6;
	u[21] |= p;
	u[22] |= p>>8;
	u[23] |= p>>16;
	u[24] |= 0xf<<4;
	p = 1<<6;
	u[24] |= p;
	u[25] |= p>>8;
}

//	c->reg[Vsiqbase + seidtovsiid(c->vsiseid)] = Vsicontig | 0;

static void
txrxinit(Ctlr *c)
{
	uchar *t, *r;

	c->tqno = c->pf*4 + 0;
	c->rqno = c->pf*4 + 0;		/* skip 1 to ignore flow director (doesn't work) */

	t = (uchar*)c->pagetab[0] + 0 + c->tqno*128;
	r = (uchar*)c->pagetab[0] + Hmcpgsz/2 + c->rqno*32;

	txcontext(c, t);
	rxcontext(c, r);
	sfence();

	c->reg[Txctl + c->tqno] = c->pf*Pfidx | Pfqueue;
	c->reg[Txtail + c->tqno] = 0;
	c->reg[Txen + c->tqno] = Enreq;
	c->tdh = c->ntd-1;

	c->rdt = 0;
	c->reg[Rxctl + c->rqno] |= Pxemode;
	c->reg[Rxtail + c->rqno] = 0;
	c->reg[Rxen + c->rqno] = Enreq;

	setpromisc(c, Bam, c->vsiseid);	/* allow broadcast pkts */
}

static void
statsreset(Ctlr *c)
{
	readstats(c, c->zstats);
	readvstats(c, c->zvstats);
}

static void
vkproc(char c, Ether *e, void(*f)(void*))
{
	char buf[KNAMELEN];

	snprint(buf, sizeof buf, "#l%d%c", e->ctlrno, c);
	kproc(buf, f, e);
}

static void
attach(Ether *e)
{
	int t;
	Ctlr *c;

	c = e->ctlr;
	lock(&c->alock);
	t = c->flag;
	c->flag |= Fstarted;
	unlock(&c->alock);
	if(t & Fstarted)
		return;
	hmcinit(c);
	linkinit(c);

	vsiinit(c);
	vsibw(c);

	statsreset(c);
	irqenable(c);
	allocblocks(c);		/* wrong place? move to rproc */
	txrxinit(c);

	vkproc('a', e, aproc);
	vkproc('r', e, rproc);
	vkproc('t', e, tproc);
}

static void
interrupt(Ureg*, void *v)
{
	u32int icr, im, pf;
	Ether *e;
	Ctlr *c;

	e = v;
	c = e->ctlr;

	ilock(&c->imlock);
	im = c->im;
	pf = c->pf;
int debug = pf==1;

	icr = c->reg[Icr + pf] & im;
//	c->reg[Idynctl + pf] = Clearpba;

if(debug)iprint("icr %.8ux %.8ux %.8ux\n", icr, c->reg[Hmerr+c->pf], c->reg[Hmerrd+c->pf]);
	if((icr & (Aprocint|Irx|Itx)) == 0)
		c->irqcnt[Cnts]++;
	if(icr&Aprocint){
		c->irqcnt[Cnta]++;
		im &= ~Aprocint;
		c->aim = icr&Aprocint;
		wakeup(&c->arendez);
	}
	if(icr&Irx){
		c->irqcnt[Cntr]++;
		c->rim = icr&Irx;
		wakeup(&c->rrendez);
	}
	if(icr&Itx){
		c->irqcnt[Cntt]++;
		c->tim = icr&Itx;
		wakeup(&c->trendez);
	}
	c->reg[Icren + pf] = im;
	c->reg[Idynctl + pf] = 3<<3 | Intena;		/* no change to coalescing; enable */
	c->im = im;
	iunlock(&c->imlock);
}

static void
hbafixup(Pcidev *p)
{
	uint i;

	i = pcicfgr32(p, PciSVID);
	if((i & 0xffff) == 0x1b52 && p->did == 1)
		p->did = i>>16;
}

static int
didmatch(int did)
{
	switch(did){
	case 0x1573:		/* sfp+ x710 */
		return xl710;	/* “eagle fountain” x710 — “fortville” */

	case 0x1572:		/* sfp+ xl710 */
	case 0x1574:		/* qemu emulation */
	case 0x157f:		/* kx */
	case 0x1583:		/* qsfp+ */
	case 0x1584:		/* qsfp+ (sample cards) */
		return xl710;	/* “spirit falls” xl710 — “fortville” */

	default:
		return -1;
	}
}

static void
scan(void)
{
	char *name;
	uintmem io;
	int type;
	void *mem;
	Ctlr *c;
	Pcidev *p;

	for(p = nil; p = pcimatch(p, 0x8086, 0);){
		hbafixup(p);
		if((type = didmatch(p->did)) == -1)
			continue;
		name = cttab[type].name;
		if(nctlr == nelem(ctlrtab)){
			print("%s: %T: too many controllers\n", name, p->tbdf);
			return;
		}
		io = p->mem[0].bar&~(uintmem)0xf;
		mem = vmap(io, p->mem[0].size);
		if(mem == nil){
			print("%s: %T: cant map bar\n", name, p->tbdf);
			continue;
		}
		c = malloc(sizeof *c);
		c->type = type;
		c->p = p;
		c->port = io;
		c->reg = (u32int*)mem;
		c->rbsz = cttab[type].mtu;
		pcisetbme(p);
		c->pf = c->reg[Portnum]&3;
		c->npf = 1;				/* wrong */
		if(reset(c)){
			print("%s: %T: cant reset\n", name, p->tbdf);
			free(c);
			vunmap(mem, p->mem[0].size);
			continue;
		}
		c->poolno = nctlr;
		c->pool = rbtab + c->poolno;
		ctlrtab[nctlr++] = c;
	}
}

static int
pnp(Ether *e)
{
	int i;
	Ctlr *c;
	static int once;

	if(once == 0){
		scan();
		once++;
	}
	for(i = 0; i<nctlr; i++){
		c = ctlrtab[i];
		if(c == nil || c->flag&Factive)
			continue;
		if(ethercfgmatch(e, c->p, c->port) == 0)
			goto found;
	}
	return -1;
found:
	c->flag |= Factive;
	e->ctlr = c;
	e->port = (uintptr)c->reg;
	e->irq = c->p->intl;
	e->tbdf = c->p->tbdf;
	e->mbps = 40000;
	e->maxmtu = c->rbsz;
	memmove(e->ea, c->ra, Eaddrlen);
	e->arg = e;
	e->attach = attach;
	e->ctl = ctl;
	e->ifstat = ifstat;
	e->interrupt = interrupt;
	e->multicast = multicast;
	e->promiscuous = promiscuous;
	e->shutdown = shutdown;
//	e->transmit = transmit;
	return 0;
}

void
etheri40link(void)
{
	addethercard("i40", pnp);
}
