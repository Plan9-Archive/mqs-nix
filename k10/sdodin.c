/*
 * marvell odin ii 88se64xx sata/sas controller
 * copyright © 2009—10 erik quanstrom
 * coraid, inc.
 */

#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"
#include "../port/sd.h"
#include <fis.h>
#include "../port/sdfis.h"
#include "../port/led.h"

#define	dprint(...)	if(debug)	print(__VA_ARGS__); else USED(debug)
#define	idprint(...)	if(idebug)	print(__VA_ARGS__); else USED(idebug)
#define	aprint(...)	if(adebug)	print(__VA_ARGS__); else USED(adebug)
#define	Pciw64(x)	(uvlong)PCIWADDR(x)
#define	Ticks		sys->ticks

/* copied from sdiahci */
enum {
	Dnull		= 0,
	Dmissing		= 1<<0,
	Dnopower	= 1<<1,
	Dnew		= 1<<2,
	Dready		= 1<<3,
	Derror		= 1<<4,
	Dreset		= 1<<5,
	Doffline		= 1<<6,
	Dportreset	= 1<<7,
	Dlast		= 9,
};

static char *diskstates[Dlast] = {
	"null",
	"missing",
	"nopower",
	"new",
	"ready",
	"error",
	"reset",
	"offline",
	"portreset",
};

static char *type[] = {
	"unknown",
	"sas",
	"sata",
};

enum{
	Nctlr		= 4,
	Nctlrdrv		= 8,
	Ndrive		= Nctlr*Nctlrdrv,
	Mbar		= 2,
	Mebar		= 4,
	Maxout		= 1,		/* outstanding cmds / drive */
	Nqueue		= 32,		/* cmd queue size */
	Qmask		= Nqueue - 1,
	Nregset		= 8,
	Rmask		= 0xffff,
	Nms		= 256,		/* drive check rate */
	Cacheline	= 64,		/* of controller, not cpu */

	/* cmd bits */
	Error		= 1<<31,
	Done		= 1<<30,
	Noverdict	= 1<<29,
	Creset		= 1<<28,
	Cmdreset	= 1<<27,
	Sense		= 1<<26,
	Timeout		= 1<<25,
	Rate		= 1<<24,
	Response	= 1<<23,
	Active		= 1<<22,

	/* pci registers */
	Phy0		= 0x40,
	Gpio		= 0x44,
	Phy1		= 0x90,
	Gpio1		= 0x94,
	Dctl		= 0xe8,

	/* phy offests */
	Phydisable	= 1<<12,
	Phyrst		= 1<<16,
	Phypdwn		= 1<<20,
	Phyen		= 1<<24,

	/* bar4 registers */
	Gctl		= 0x004/4,
	Gis		= 0x008/4,	/* global interrupt status */
	Pi		= 0x00c/4,	/* ports implemented */
	Flashctl		= 0x030/4,	/* spi flash control */
	Flashcmd	= 0x034/4,	/* flash wormhole */
	Flashdata	= 0x038/4,
	I²cctl		= 0x040/4,	/* i²c control */
	I²ccmd		= 0x044/4,
	I²cdata		= 0x048/4,
	Ptype		= 0x0a0/4,	/* 15:8 auto detect enable; 7:0 sas=1. sata=0 */
	Portcfg0		= 0x100/4,	/* 31:16 register sets 31:16 */
	Portcfg1		= 0x104/4,	/* 31:16 register sets 15:8 tx enable; 7 rx enable */
	Clbase		= 0x108/4,	/* cmd list base; 64 bits */
	Fisbase		= 0x110/4,	/* 64 bits */
	Dqcfg		= 0x120/4,	/* bits 11:0 specify size */
	Dqbase		= 0x124/4,
	Dqwp		= 0x12c/4,	/* delivery queue write pointer */
	Dqrp		= 0x130/4,
	Cqcfg		= 0x134/4,
	Cqbase		= 0x138/4,
	Cqwp		= 0x140/4,	/* hw */
	Coal		= 0x148/4,
	Coalto		= 0x14c/4,	/* coal timeout µs */
	Cis		= 0x150/4,	/* centeral irq status */
	Cie		= 0x154/4,	/* centeral irq enable */
	Csis		= 0x158/4,	/* cmd set irq status */
	Csie		= 0x15c/4,
	Cmda		= 0x1b8/4,
	Cmdd		= 0x1bc/4,
	Gpioa		= 0x270/4,
	Gpiod		= 0x274/4,
	Gpiooff		= 0x100,		/* second gpio offset */

	/* port conf registers; mapped through wormhole */
	Pinfo		= 0x000,
	Paddr		= 0x004,
	Painfo		= 0x00c,		/* attached device info */
	Pawwn		= 0x010,
	Psatactl		= 0x018,
	Pphysts		= 0x01c,
	Psig		= 0x020,		/* 16 bytes */
	Perr		= 0x030,
	Pcrcerr		= 0x034,
	Pwidecfg		= 0x038,
	Pwwn		= 0x080,		/* 12 wwn + ict */

	/* port cmd registers; mapped through “cmd” wormhole */
	Ci		= 0x040,		/* cmd active (16) */
	Task		= 0x080,
	Rassoc		= 0x0c0,
	Phytimer		= 0x118,
	Sasctl0		= 0x124,
	Sasctl1		= 0x128,
	Sasctl2		= 0x12c,
	Sasctl3		= 0x130,
	Pwdtimer	= 0x13c,

	/* “vendor specific” wormhole */
	Phymode	= 0x001,

	/* gpio wormhole */
	Sgconf0		= 0x000,
	Sgconf1		= 0x004,
	Sgclk		= 0x008,
	Sgconf3		= 0x00c,
	Sgis		= 0x010,		/* interrupt set */
	Sgie		= 0x014,		/* interrupt enable */
	Drivesrc		= 0x020,		/* 4 drives/register; 4 bits/drive */
	Drivectl		= 0x038,		/* same deal */

	/* Gctl bits */
	Reset		= 1<<0,
	Intenable	= 1<<1,

	/* Portcfg0/1 bits */
	Regen		= 1<<16,	/* enable sata regsets 31:16 or 15:0 */
	Xmten		= 1<<8,	/* enable port n transmission */
	Dataunke	= 1<<3,
	Rsple		= 1<<2,	/* response frames in le format */
	Oabe		= 1<<1,	/* oa frame in big endian format */
	Framele		= 1<<0,	/* frame contents in le format */

	Allresrx		= 1<<7,	/* receive all responses */
	Stpretry		= 1<<6,
	Cmdirq		= 1<<5,	/* 1 == self clearing */
	Fisen		= 1<<4,	/* enable fis rx */
	Errstop		= 1<<3,	/* set -> stop on ssp/smp error */
	Resetiss		= 1<<1,	/* reset cmd issue; self clearing */
	Issueen		= 1<<0,

	/* Dqcfg bits */
	Dqen		= 1<<16,

	/* Cqcfg bits */
	Noattn		= 1<<17,	/* don't post entries with attn bit */
	Cqen		= 1<<16,

	/* Cis bits */
	I2cirq		= 1<<31,
	Swirq1		= 1<<30,
	Swirq0		= 1<<29,
	Prderr		= 1<<28,
	Dmato		= 1<<27,
	Parity		= 1<<28,	/* parity error; fatal */
	Slavei2c		= 1<<25,
	Portstop		= 1<<16,	/* bitmapped */
	Portirq		= 1<<8,	/* bitmapped */
	Srsirq		= 1<<3,
	Issstop		= 1<<1,
	Cdone		= 1<<0,
	Iclr		= Swirq1 | Swirq0,

	/* Pis bits */
	Caf		= 1<<29,	/* clear affiliation fail */
	Sync		= 1<<25,	/* sync during fis rx */
	Phyerr		= 1<<24,
	Stperr		= 1<<23,
	Crcerr		= 1<<22,
	Linktx		= 1<<21,
	Linkrx		= 1<<20,
	Martianfis	= 1<<19,
	Anot		= 1<<18,	/* async notification */
	Bist		= 1<<17,
	Sigrx		= 1<<16,	/* native sata signature rx */
	Phyunrdy	= 1<<12,	/* phy went offline*/
	Uilong		= 1<<11,
	Uishort		= 1<<10,
	Martiantag	= 1<<9,
	Bnot		= 1<<8,	/* broadcast noticication */
	Comw		= 1<<7,
	Portsel		= 1<<6,
	Hreset		= 1<<5,
	Phyidto		= 1<<4,
	Phyidfail		= 1<<3,
	Phyidok		= 1<<2,
	Hresetok		= 1<<1,
	Phyrdy		= 1<<0,

	Pisataup		= Phyrdy | Comw | Sigrx,
	Pisasup		= Phyrdy | Comw | Phyidok,
	Piburp		= Sync | Phyerr | Stperr | Crcerr | Martiantag,
	Pireset		= Phyidfail | Bnot | Phyunrdy | Bist |
				Anot | Martianfis | Bist | Phyidto |
				Hreset,
	Piunsupp	= Portsel,

	/* Psc bits */
	Sphyrdy		= 1<<20,
	Linkrate		= 1<<18,	/* 4 bits */
	Maxrate		= 1<<12,
	Minrate		= 1<<8,
	Sreset		= 1<<3,
	Sbnote		= 1<<2,
	Shreset		= 1<<1,
	Sphyrst		= 1<<0,

	/* Painfo bits */
	Issp		= 1<<19,
	Ismp		= 1<<18,
	Istp		= 1<<17,
	Itype		= 1<<0,	/* two bits */

	/* Psatactl bits */
	Powerctl		= 1<<30,	/* 00 wake; 10 partial 01 slumb */
	Srst		= 1<<29,	/* soft reset */
	Power		= 1<<28,
	Sportsel		= 1<<24,
	Dmahon		= 1<<22,
	Srsten		= 1<<20,
	Dmaxfr		= 1<<18,

	/* Pphysts bits */
	Ennot		= 1<<2,	/* enable notify */
	Notmsk		= 3<<0,
	Notres2		= 3<<0,
	Notres1		= 2<<0,	/* notify (reserve 1) */
	Notpl		= 1<<0,	/* notify (power loss expected) */
	Notspinup	= 0<<0,	/* notify (enable spinup) */
	
	/* phy status bits */
	Phylock		= 1<<9,
	Nspeed		= 1<<4,
	Psphyrdy		= 1<<2,

	/* Task bits; modeled after ahci */
	Eestatus		= 0xff<<24,
	Asdbs		= 1<<18,
	Apio		= 1<<17,
	Adhrs		= 1<<16,
	Eerror		= 0xff<<8,
	Estatus		= 0xff,

	/* Phymode bits */
	Pmnotify		= 1<<24,
	Pmnotifyen	= 1<<23,

	/* Sgconf0 bits */
	Autolen		= 1<<24,	/* 8 bits */
	Manlen		= 1<<16,	/* 8 bits */
	Sdincapt		= 1<<8,	/* capture sdatain on + edge */
	Sdoutch		= 1<<7,	/* change sdataout on - edge
	Sldch		= 1<<6,	/* change sload on - edge
	Sdoutivt		= 1<<5,	/* invert sdataout polarity */
	Ldivt		= 1<<4,
	Sclkivt		= 1<<3,
	Blinkben		= 1<<2,	/* enable blink b */
	Blinkaen		= 1<<1,	/* enable blink a */
	Sgpioen		= 1<<0,

	/* Sgconf1 bits; 4 bits each */
	Sactoff		= 1<<28,	/* stretch activity off; 0/64 - 15/64 */
	Sacton		= 1<<24,	/* 1/64th - 16/64 */
	Factoff		= 1<<20,	/* 0/8 - 15/8; default 1 */
	Facton		= 1<<16,	/* 0/4 - 15/4; default 2 */
	Bhi		= 1<<12,	/* 1/8 - 16/8 */
	Blo		= 1<<8,	/* 1/8 - 16/8 */
	Ahi		= 1<<4,	/* 1/8 - 16/8 */
	Alo		= 1<<0,	/* 1/8 - 16/8 */

	/* Sgconf3 bits */
	Autopat		= 1<<20,	/* 4 bits of start pattern */
	Manpat		= 1<<16,
	Manrep		= 1<<4,	/* repeats; 7ff ≡ ∞ */
	Sdouthalt	= 0<<2,
	Sdoutman	= 1<<2,
	Sdoutauto	= 2<<2,
	Sdoutma		= 3<<2,
	Sdincapoff	= 0<<0,
	Sdinoneshot	= 1<<0,
	Sdinrep		= 2<<0,

	/* Sgie Sgis bits */
	Sgreprem	= 1<<8,	/* 12 bits; not irq related */
	Manrep0		= 1<<1,	/* write 1 to clear */
	Capdone	= 1<<0,	/* capture done */

	/* drive control bits (repeated 4x per drive) */
	Aled		= 1<<5,	/* 3 bits */
	Locled		= 1<<3,	/* 2 bits */
	Errled		= 1<<0,	/* 3 bits */
	Llow		= 0,
	Lhigh		= 1,
	Lblinka		= 2,
	Lblinkaneg	= 3,
	Lsof		= 4,
	Leof		= 5,
	Lblinkb		= 6,
	Lblinkbneg	= 7,

	/* cmd queue bits */
	Dssp		= 1<<29,
	Dsmp		= 2<<29,
	Dsata		= 3<<29,	/* also stp */
	Ditor		= 1<<28,	/* initiator */
	Dsatareg		= 1<<20,
	Dphyno		= 1<<12,
	Dcslot		= 1,

	/* completion queue bits */
	Cgood		= 1<<23,	/* ssp only */
	Cresetdn		= 1<<21,
	Crx		= 1<<20,	/* target mode */
	Cattn		= 1<<19,
	Crxfr		= 1<<18,
	Cerr		= 1<<17,
	Cqdone		= 1<<16,
	Cslot		= 1<<0,	/* 12 bits */

	/* error bits — first word */
	Eissuestp	= 1<<31,	/* cmd issue stopped */
	Epi		= 1<<30,	/* protection info error */
	Eoflow		= 1<<29,	/* buffer overflow */
	Eretry		= 1<<28,	/* retry limit exceeded */
	Eufis		= 1<<27,
	Edmat		= 1<<26,	/* dma terminate */
	Esync		= 1<<25,	/* sync rx during tx */
	Etask		= 1<<24,
	Ererr		= 1<<23,	/* r error received */

	Eroff		= 1<<20,	/* read data offset error */
	Exoff		= 1<<19,	/* xfer rdy offset error */
	Euxr		= 1<<18,	/* unexpected xfer rdy */
	Exflow		= 1<<16,	/* buffer over/underflow */
	Elock		= 1<<15,	/* interlock error */
	Enak		= 1<<14,
	Enakto		= 1<<13,
	Enoak		= 1<<12,	/* conn closed wo nak */
	Eopento		= 1<<11,	/* open conn timeout */
	Epath		= 1<<10,	/* open reject - path blocked */
	Enodst		= 1<<9,	/* open reject - no dest */
	Estpbsy		= 1<<8,	/* stp resource busy */
	Ebreak		= 1<<7,	/* break while sending */
	Ebaddst		= 1<<6,	/* open reject - bad dest */
	Ebadprot	= 1<<5,	/* open reject - proto not supp */
	Erate		= 1<<4,	/* open reject - rate not supp */
	Ewdest		= 1<<3,	/* open reject - wrong dest */
	Ecreditto	= 1<<2,	/* credit timeout */
	Edog		= 1<<1,	/* watchdog timeout */
	Eparity		= 1<<0,	/* buffer parity error */
	Saserr		= Ecreditto | Ewdest | Ebadprot | Ererr,

	/* sas ctl cmd header bits */
	Ssptype		= 1<<5,	/* 3 bits */
	Ssppt		= 1<<4,	/* build your own header *.
	Firstburst	= 1<<3,	/* first burst */
	Vrfylen		= 1<<2,	/* verify length */
	Tlretry		= 1<<1,	/* transport layer retry */
	Piren		= 1<<0,	/* pir present */

	/* sata ctl cmd header bits */
	Lreset		= 1<<7,
	Lfpdma		= 1<<6,	/* first-party dma */
	Latapi		= 1<<5,
	Lpm		= 1<<0,	/* 4 bits */

	Sspcmd		= 0*Ssptype,
	Ssptask		= 1*Ssptype,
	Sspxfrdy		= 4*Ssptype,
	Ssprsp		= 5*Ssptype,
	Sspread		= 6*Ssptype,
	Sspwrite		= 7*Ssptype,

	/* memory offsets and sizes */
	Fis0		= 0x800,		/* 0x400 on 94xx vanir */
};

/* following ahci */
typedef struct {
	u32int	dba;
	u32int	dbahi;
	u32int	pad;
	u32int	count;
} Aprdt;

typedef struct {
	union{
		struct{
			uchar	cfis[0x40];
			uchar	atapi[0x20];
		};
		struct{
			uchar	mfis[0x40];
		};
		struct{
			uchar	sspfh[0x18];
			uchar	sasiu[0x40];
		};
	};
} Ctab;

/* open address frame */
typedef struct {
	uchar	oaf[0x28];
	uchar	fb[4];
} Oaf;

/* status buffer */
typedef struct {
	uchar	error[8];
	uchar	rsp[0x400];
} Statb;

typedef struct {
	uchar	satactl;
	uchar	sasctl;
	uchar	len[2];

	uchar	fislen[2];
	uchar	maxrsp;
	uchar	d0;

	uchar	tag[2];
	uchar	ttag[2];

	uchar	dlen[4];
	uchar	ctab[8];
	uchar	oaf[8];
	uchar	statb[8];
	uchar	prd[8];

	uchar	d3[16];
} Cmdh;

typedef struct Cmd Cmd;
struct Cmd {
	Rendez;
	uint	cflag;

	Cmdh	*cmdh;
	Ctab;
	Oaf;
	Statb;
	Aprdt;
};

typedef struct Drive Drive;
typedef struct Ctlr Ctlr;

struct Drive {
	Lock;
	QLock;
	Ctlr	*ctlr;
	SDunit	*unit;
	char	name[16];

	Cmd	*cmd;

	uchar	state;
//	uchar	type;

	Sfisx;
	Ledport;

	/* hotplug info */
	uint	lastseen;
	uint	intick;
	uint	wait;

	uint	driveno;
};

struct Ctlr {
	Lock;
	uchar	enabled;
	SDev	*sdev;
	Pcidev	*pci;
	uint	*reg;

	uchar	mem[3*Cacheline + (2*Nqueue + 1)*sizeof(uint)];
	uint	*dq;			/* Nqueue */
	uint	dqwp;
	uint	*cq;			/* Nqueue + 1 */
	uint	cqrp;
	Cmdh	*cl;
	uchar	*fis;
	Cmd	*cmdtab;

	Drive	drive[Nctlrdrv];
	uint	ndrive;
	uint	prderr;

	void	*vector;
};

static	Ctlr	msctlr[Nctlr];
static	SDev	sdevs[Nctlr];
static	uint	nmsctlr;
static	Drive	*msdrive[Ndrive];
static	uint	nmsdrive;
static	int	debug			= 0;
static	int	idebug			= 1;
static	int	adebug			= 0;
static	uint	 olds[Nctlr*Nctlrdrv];
	SDifc	sdodinifc;

/* a good register is hard to find */
static	int	pis[] = {
	0x160/4, 0x168/4, 0x170/4, 0x178/4,
	0x200/4, 0x208/4, 0x210/4, 0x218/4,
};
static	int	pcfg[] = {
	0x1c0/4, 0x1c8/4, 0x1d0/4, 0x1d8/4,
	0x230/4, 0x238/4, 0x240/4, 0x248/4,
};
static	int	psc[] = {
	0x180/4, 0x184/4, 0x188/4, 0x18c/4,
	0x220/4, 0x224/4, 0x228/4, 0x22c/4,
};
static	int	vscfg[] = {
	0x1e0/4, 0x1e8/4, 0x1f0/4, 0x1f8/4,
	0x250/4, 0x258/4, 0x260/4, 0x268/4,
};
#define	sstatus(d)	(d)->ctlr->reg[psc[(d)->driveno]]

static char*
dstate(uint s)
{
	int i;

	for(i = 0; s; i++)
		s >>= 1;
	return diskstates[i];
}

static char*
dnam(Drive *d)
{
	if(d->unit)
		return d->unit->name;
	return d->name;
}

static char*
cnam(Ctlr *c)
{
	return c->sdev->ifc->name;
}

static int phyrtab[] = {Phy0, Phy1};
static void
phyenable(Ctlr *c, Drive *d)
{
	uint i, u, reg, m;

	i = d->driveno;
	reg = phyrtab[i > 3];
	i &= 3;
	i = 1<<i;
	u = pcicfgr32(c->pci, reg);
	m = i*(Phypdwn | Phydisable | Phyen);
	if((u & m) == Phyen)
		return;
	m = i*(Phypdwn | Phydisable);
	u &= ~m;
	u |= i*Phyen;
	pcicfgw32(c->pci, reg, u);
}

static void
regtxreset(Drive *d)
{
	uint i, u, m;
	Ctlr *c = d->ctlr;

	i = d->driveno;
	u = c->reg[Portcfg1];
	m = (Regen|Xmten)<<i;
	u &= ~m;
	c->reg[Portcfg1] = u;
	delay(1);
	c->reg[Portcfg1] = u | m;
}

static void
phykick(Drive *d)
{
	uint i, u, reg;
	Ctlr *c;

	c = d->ctlr;
	phyenable(c, d);

	i = d->driveno;
	reg = phyrtab[i > 3];
	i &= 3;
	u = pcicfgr32(c->pci, reg);
	pcicfgw32(c->pci, reg, u | Phyrst<<i);
	delay(5);
	pcicfgw32(c->pci, reg, u);
}

static void
phyreset(Drive *d)
{
	phykick(d);
	sstatus(d) |= Shreset;
	while(sstatus(d) & Shreset);
		;
}

static void
reset(Drive *d)
{
	regtxreset(d);
	phyreset(d);
}

/*
 * sata/sas register reads through wormhole
 */
static uint
ssread(Ctlr *c, int port, uint r)
{
	c->reg[Cmda] = r + 4*port;
	return c->reg[Cmdd];
}

static void
sswrite(Ctlr *c, int port, int r, uint u)
{
	c->reg[Cmda] = r + 4*port;
	c->reg[Cmdd] = u;
}

/*
 * port configuration r/w through wormhole
 */
static uint
pcread(Ctlr *c, uint port, uint r)
{
	c->reg[pcfg[port]] = r;
	return c->reg[pcfg[port] + 1];
}

static uvlong
pcread64(Ctlr *c, uint port, int r)
{
	uvlong u;

	u = pcread(c, port, r + 0);
	u |= (uvlong)pcread(c, port, r + 4)<<32;
	return u;
}

static void
pcwrite(Ctlr *c, uint port, uint r, uint u)
{
	c->reg[pcfg[port] + 0] = r;
	c->reg[pcfg[port] + 1] = u;
}

/*
 * vendor specific r/w through wormhole
 */
static uint
vsread(Ctlr *c, uint port, uint r)
{
	c->reg[vscfg[port]] = r;
	return c->reg[vscfg[port] + 1];
}

static void
vswrite(Ctlr *c, uint port, uint r, uint u)
{
	c->reg[vscfg[port] + 0] = r;
	c->reg[vscfg[port] + 1] = u;
}

/*
 * gpio wormhole
 */
static uint
gpread(Ctlr *c, uint r)
{
	c->reg[Gpioa] = r;
	return c->reg[Gpiod];
}

static void
gpwrite(Ctlr *c, uint r, uint u)
{
	c->reg[Gpioa] = r;
	c->reg[Gpiod] = u;
}

static uint*
getsigfis(Drive *d, uint *fis)
{
	uint i;

	for(i = 0; i < 4; i++)
		fis[i] = pcread(d->ctlr, d->driveno, Psig + 4*i);
	return fis;
}

static uint
getsig(Drive *d)
{
	uint fis[4];

	return fistosig((uchar*)getsigfis(d, fis));
}

static uint
ci(Drive *d)
{
	uint i;

	i = 1<<d->driveno;
	return ssread(d->ctlr, 0, Ci) & i;
}

static void
unsetci(Drive *d)
{
	uint i;

	i = 1<<d->driveno;
	sswrite(d->ctlr, 0, Ci, i);
	while(ci(d))
		microdelay(1);
}

static uint
gettask(Drive *d)
{
	return ssread(d->ctlr, d->driveno, Task);
}

static void
tprint(Drive *d, uint t)
{
	uint s;

	s = sstatus(d);
	dprint("%s: err task %ux sstat %ux\n", dnam(d), t, s);
}

static int
cmddone(void *v)
{
	Cmd *x;

	x = v;
	return (x->cflag & Done) != 0;
}

static int
wait0(Cmd *x, int ms)
{
	uint tk0;

	tk0 = Ticks;
	if(up){
		while(waserror())
			;
		tsleep(x, cmddone, x, ms);
		poperror();
		ms -= TK2MS(Ticks - tk0);
	}else
		while(ms-- && !cmddone(x))
			delay(1);
	return ms;
}

static int
mswait(Drive *d, Cmd *x, int ms)
{
	uint u, tk0;

	tk0 = Ticks;
	ms = wait0(x, ms);
	if((x->cflag & Done) == 0){
	}
	ilock(d->ctlr);
	u = x->cflag;
	x->cflag = 0;
	iunlock(d->ctlr);
	if(u == (Done | Active))
		return 0;
	if((u & Done) == 0){
		u |= Noverdict | Creset | Cmdreset | Timeout;
		print("%s: cmd timeout ms:%d/%ld %ux\n", dnam(d), ms, Ticks - tk0, u);
	}
	return u;
}

static void
setstate(Drive *d, int state)
{
	ilock(d);
	d->state = state;
	iunlock(d);
}

static void
esleep(int ms)
{
	if(waserror())
		return;
	tsleep(&up->sleep, return0, 0, ms);
	poperror();
}

static int
isready(Drive *d)
{
	ulong s, δ;

	if(d->state & (Dreset | Dportreset /*| Dnew*/))
		return 1;
	δ = TK2MS(Ticks - d->lastseen);
	if(d->state == Dnull || δ > 10*1000)
		return -1;
	ilock(d);
	s = sstatus(d);
	iunlock(d);
	if((s & Sphyrdy) == 0 && δ > 1500)
		return -1;
	if(d->state & (Dready | Dnew) && (s & Sphyrdy))
		return 0;
	return -1;
}

static int
lockready(Drive *d, ulong tk)
{
	int r;

	for(;;){
		qlock(d);
		if((r = isready(d)) == 0)
			return r;
		qunlock(d);
		if(r == -1)
			return r;
//		if(Ticks >= tk + 10)
		if(tk - Ticks - 10 >= 1ul<<31)
			return -1;
		esleep(10);
	}
}

static int
command(Drive *d, uint cmd, int ms)
{
	uint s, n, m, i;
	Ctlr *c;

	c = d->ctlr;
	i = d->driveno;
	m = 1<<i;
	n = cmd | Ditor | i*Dsatareg | m*Dphyno | i*Dcslot;
//	print("cqwp\t%.8ux : n %ux : d%d; \n", c->cq[0], n, i);
	/*
	 * xinc doesn't return the previous value and i can't
	 * figure out how to do this without a lock
	 *	s = _xinc(&c->dqwp);
	 */
	ilock(c);
	d->cmd->cflag = Active;
	s = c->dqwp++;
	c->dq[s&Qmask] = n;
	c->reg[Dqwp] = s&Qmask;
	iunlock(c);
//	print("	dq slot %d\n", s);
	d->intick = Ticks;		/* move to mswait? */
	return mswait(d, d->cmd, ms);
}

static int
buildfis(Drive *d, SDreq *r, void *data, int n)
{
	Aprdt *p;
	Cmd *x;
	Cmdh *h;

	x = d->cmd;
	memmove(x->cfis, r->cmd, r->clen);

	h = x->cmdh;
	memset(h, 0, 16);
	h->fislen[0] = 5;
	h->len[0] = 0;

	if(data != nil){
		h->len[0] = 1;
		p = x;
		p->dba = Pciwaddrl(data);
		p->dbahi = Pciwaddrh(data);
		p->count = n;
	}
	return command(d, Dsata, r->timeout);
}

enum{
	Rnone	= 1,
	Rdma	= 0x00,		/* dma setup; length 0x1b */
	Rpio	= 0x20,		/* pio setup; length 0x13 */
	Rd2h	= 0x40,		/* d2h register;length 0x13 */
	Rsdb	= 0x58,		/* set device bits; length 0x08 */
};

static uint fisotab[8] = {
[0]	Rnone,
[1]	Rd2h,
[2]	Rpio,
[3]	Rnone,
[4]	Rsdb,
[5]	Rnone,
[6]	Rnone,
[7]	Rnone,
};

static uint
fisoffset(Drive *d, int mustbe)
{
	uint t, r;

	t = gettask(d) & 0x70000;
	r = fisotab[t >> 16];
	if(r == Rnone || (mustbe != 0 && r != mustbe))
		return 0;
	return Fis0 + 0x100*d->driveno + r;
}

/* need to find a non-atapi-specific way of doing this */
static uint
atapixfer(Drive *d, uint n)
{
	uchar *u;
	uint i, x;

	if((i = fisoffset(d, Rd2h)) == 0)
		return 0;
	u = d->ctlr->fis + i;
	x = u[Flba16]<<8 | u[Flba8];
	if(x > n){
		x = n;
		print("%s: atapixfer %ux %ux\n", dnam(d), x, n);
	}
	return x;
}

static int
buildpkt(Drive *d, SDreq *r, void *data, int n)
{
	int rv;
	Aprdt *p;
	Cmd *x;
	Cmdh *h;

	x = d->cmd;
	atapirwfis(d, x->cfis, r->cmd, r->clen, n);

	h = x->cmdh;
	memset(h, 0, 16);
	h->satactl = Latapi;
	h->fislen[0] = 5;
	h->len[0] = 1;		/* one prdt entry */

	if(data != nil){
		p = x;
		p->dba = Pciwaddrl(data);
		p->dbahi = Pciwaddrh(data);
		p->count = n;
	}
	rv = command(d, Dsata, r->timeout);
	if(rv == 0)
		r->rlen = atapixfer(d, n);
	return rv;
}

static int
settxmode(Drive *d, uchar f)
{
	Cmd *x;
	Cmdh *h;

	x = d->cmd;
	if(txmodefis(d, x->cfis, f) == -1)
		return 0;

	h = x->cmdh;
	memset(h, 0, 16);
	h->fislen[0] = 5;

	return command(d, Dsata, 3*1000);
}

/* sas fises */
static int
sasfis(Cfis*, uchar *c, SDreq *r)
{
	memmove(c, r->cmd, r->clen);
	if(r->clen < 16)
		memset(c + r->clen, 0, 16 - r->clen);
	return 0;
}

/* sam3 §4.9.4 single-level lun structure */
static void
scsilun8(uchar *c, int l)
{
	memset(c, 0, 8);
	if(l < 255)
		c[1] = l;
	else if(l < 16384){
		c[0] = 1<<6 | l>>8;
		c[1] = l;
	}else
		print("bad lun %d\n", l);
}

static void
iuhdr(SDreq *r, uchar *c, int fburst)
{
	scsilun8(c, r->lun);
	c[8] = 0;
	c[9] = 0;
	if(fburst)
		c[9] = 0x80;
}

static void
ssphdr(Cfis *x, uchar *c, int ftype)
{
	memset(c, 0, 0x18);
	c[0] = ftype;
	sasbhash(c + 1, x->tsasaddr);
	sasbhash(c + 5, x->ssasaddr);
}

/* debugging */
static void
dump(char *tag, uchar *u, uint n)
{
	uint i;

	print("%s %d:\n", tag, n);
	if(n > 100)
		n = 100;
	for(i = 0; i < n; i += 4){
		print("%.2d  %.2ux%.2ux%.2ux%.2ux", i, u[i], u[i + 1], u[i + 2], u[i + 3]);
		print("\n");
	}
}

static int
classifykey(Drive *d, int asckey)
{
	uint u;

	if(asckey == 0x020411){
		dprint("need notify (enable spinup)\n");
		u = pcread(d->ctlr, d->driveno, Pphysts);
		u &= ~(Ennot | Notmsk);
		pcwrite(d->ctlr, d->driveno, Pphysts, u | Ennot | Notspinup);
		delay(1);
		pcwrite(d->ctlr, d->driveno, Pphysts, u);
		return SDretry;
	}
	return SDcheck;
}

/* spc3 §4.5 */
static int
sasrspck(Drive *d, SDreq *r, int min)
{
	int rv;
	uchar *u, *s;
	uint l, fmt, n, keyasc;

	u = d->cmd->rsp;
	s = u + 24;
	dprint("status %d datapres %d\n", u[11], u[10]);
	switch(u[10]){
	case 1:
		l = getbe(u + 20, 4);
		/*
		 * this is always a bug because we don't do
		 * task mgmt
		 */
		print("%s: bug: task data %d min %d\n", dnam(d), l, min);
		return SDcheck;
	case 2:
		l = getbe(u + 16, 4);
		n = sizeof r->sense;
		if(l < n)
			n = l;
		memmove(r->sense, s, n);
		fmt = s[0] & 0x7f;
		keyasc = (s[2] & 0xf)<<16 | s[12]<<8 | s[13];
		rv = SDcheck;
		/* spc3 §4.5.3; 0x71 is deferred. */
		if(n >= 18 && (fmt == 0x70 || fmt == 0x71)){
			rv = classifykey(d, keyasc);
			r->flags |= SDvalidsense;
			dprint("%s: sense %.2ux %.6ux %d\n",
				dnam(d), r->sense[0], keyasc, rv);
		}else
			dump("sense", s, l);
		return rv;
	default:
		print("%s: sasrspck: spurious\n", dnam(d));
		dump("iu", u, 24);
		dump("sense", s, 0x30);
		return SDcheck;
	}
}

static void
setreqsense(SDreq *r, uint key)
{
	uchar *s;

	s = r->sense;
	s[0] = 0x80 | 0x70;	/* valid; fixed-format */
	s[2] = key>>16 & 0xf;
	s[12] = key >> 8;
	s[13] = key;
	r->flags |= SDvalidsense;
	r->status = SDcheck;
}

static int
buildsas(Drive *d, SDreq *r, uchar *data, int n)
{
	int w, fburst;
	Aprdt *p;
	Cmd *x;
	Cmdh *h;

	fburst = 0;
	x = d->cmd;
	/* ssphdr(d, x->sspfh, 6); */
	iuhdr(r, x->sasiu, fburst);
	w = 0;
	if(r->clen > 16)
		w = r->clen - 16 + 3>> 2;
	x->sasiu[11] = w;
	sasfis(d, x->sasiu + 12, r);

	h = x->cmdh;
	memset(h, 0, 16);
	h->sasctl = Tlretry | /*Vrfylen |*/ Sspcmd | fburst;
	h->fislen[0] = sizeof x->sspfh + 12 + 16 + 4*w >> 2;
	h->maxrsp = 0xff;
	if(n)
		h->len[0] = 1;
	*(uint*)h->dlen = n;

	if(n){
		p = x;
		p->dba = Pciwaddrl(data);
		p->dbahi = Pciwaddrh(data);
		p->count = n;
	}
	aprint("sascmd %.2ux [%d] %d\n", r->cmd[0], r->clen, n);
	switch(w = command(d, Dssp, r->timeout)){
	case 0:
		r->status = sdsetsense(r, SDok, 0, 0, 0);	/* botch */
		return 0;
	case Response | Done | Active:
		r->status = sasrspck(d, r, 0);
		if(r->status == SDok)
			return 0;
		r->flags |= SDvalidsense;
		return w | Sense;
	default:
		if(w & Rate)
			r->status = SDrate;
		return w;
	}
}

static void
issuerst(Drive *d)
{
	uint x, i;
	Ctlr *c;

	c = d->ctlr;
	i = d->driveno;
	x = pcread(c, i, Psatactl);
	pcwrite(c, i, Psatactl, x | Srst | Srsten);
	while(pcread(c, i, Psatactl) & Srst)
		;
}

static void
srsirq(Drive *d)
{
	issuerst(d);
	regtxreset(d);
}

static void
unstop(Drive *d)
{
	int i;
	Ctlr *c;

	c = d->ctlr;
	i = 1<<d->driveno;
	c->reg[Csis] = i;
}

static uint
analyze(Drive *d, Statb *b)
{
	uint u, r, t;

	r = 0;
	u = *(uint*)b->error;
	if(u & (Eissuestp | Edog)){
		dprint("%s: issue stop/timeout %.8ux\n", dnam(d), u);
		r |= Done | Noverdict;
		unsetci(d);
		unstop(d);
	}
	if(u & Etask && (d->feat & Datapi) == 0){
		t = gettask(d);
		if(t & 1)
			tprint(d, t);
		if(t & Efatal<<8 || t & (ASbsy|ASdrq))
			r |= Noverdict|Cmdreset;
		if(t&Adhrs && t&33)
			r |= Noverdict|Cmdreset;
		else
			r |= Error;
	}
	if(u & Erate){
		dprint("%s: erate\n", dnam(d));
		r |= Rate | Error;
	}
	if(u & Eopento)
		r = Noverdict | Cmdreset;
	if(u & (Ererr | Ebadprot)){
		print("%s: sas error %.8ux\n", dnam(d), u);
		r |= Error;
	}
	if(u & ~(Eopento | Ebadprot | Ererr | Etask | Eissuestp | Edog))
		print("%s: analyze leftovers: %.8ux\n", dnam(d), u);
	return r;
}

static void
updatedone(Ctlr *c)
{
	uint a, e, i, o, u, y, slot;
	Cmd *x;
	Drive *d;

redux:
	o = e = c->cq[0];
	if(e == 0xfff)
		return;
	if(e > Qmask)
		print("sdodin: bug: bad cqrp %ux\n", e);
	e = e+1 & Qmask;
	for(i = c->cqrp; i != e; i = i+1 & Qmask){
		u = c->cq[1 + i];
	//	c->cq[1 + i] = 0;
		slot = u & 0xfff;
		u &= ~slot;
		d = c->drive + slot;
  		x = d->cmd;
		y = 0;
		if(u & Cqdone){
			y |= Done;
			u &= ~Cqdone;
		}
		if(u & (Crxfr | Cgood)){
			if((u & Cgood) == 0)
				y |= Response;
			u &= ~(Crxfr | Cgood);
		}
		if(u & Cerr){
			a = analyze(d, x);
			y |= Done | a;
			u &= ~Cerr;
		}
		if(y & Done && (x->cflag&(Done|Active)) == Active){
			x->cflag |= y;
			wakeup(x);
		}
		else
			print("%s: donerace %.8ux\n", dnam(d), x->cflag);
		if(u)
			print("%s: odd bits %.8ux\n", dnam(d), u);
	}
//	if(i == c->cqrp)
//		print("sdodin: spur done %d %d\n", i, Qmask);
	c->cqrp = i;
	if(o != c->cq[0])
		goto redux;
}

static void
updatedrive(Drive *d)
{
	uint cause, s0, s1, ewake;
	Cmd *x;
	static uint last, tk;

	ewake = 0;
	cause = d->ctlr->reg[pis[d->driveno]];
	d->ctlr->reg[pis[d->driveno]] = cause;
	x = d->cmd;
	s0 = d->state;
	s1 = s0;

	if(last != cause || Ticks - tk > 5*1000){
		dprint("%s: ca %ux ta %ux\n", dnam(d), cause, gettask(d));
		tk = Ticks;
	}
	if(cause & Phyerr && s0 == Dready){
		dprint("%s: phyerr\n", dnam(d));			/* XXX */
		sstatus(d) |= Shreset;
		while(sstatus(d) & Shreset);
			;
	}
	if(cause & (Phyunrdy | Phyidto | Pisataup | Pisasup)){
		if(cause == (Phyrdy | Comw)){
			d->type = 0;
			s1 = Dnopower;
		}
		switch(cause & (Phyunrdy | Phyidto | Phyidok | Sigrx)){
		case Phyunrdy:
			s1 = Dmissing;
			if(sstatus(d) & Sphyrdy){
				if(d->type != 0)
					s1 = Dnew;
				else
					s1 = Dreset;
			}
			break;
		case Phyidto:
			d->type = 0;
			s1 = Dmissing;
			break;
		case Phyidok:
			d->type = Sas;
			s1 = Dnew;
			break;
		case Sigrx:
			d->type = Sata;
			s1 = Dnew;
			break;
		}
		dprint("%s: %s → %s [Apcrs] %s %ux\n", dnam(d), dstate(s0),
			dstate(s1), type[d->type], sstatus(d));
		if(s0 == Dready && s1 != Dready)
			idprint("%s: pulled\n", dnam(d));
		if(s1 != Dready || ci(d))
			ewake |= Done | Noverdict;
	}else if(cause & Piburp)
		ewake |= Done | Noverdict | Cmdreset;
	else if(cause & Pireset)
		ewake |= Done | Noverdict | Creset;
	else if(cause & Piunsupp){
		print("%s: unsupported h/w: %.8ux\n", dnam(d), cause);
		ewake |= Done | Error;
		d->type = 0;
		s1 = Doffline;
	}else if(cause & (Linkrx | Linktx)){
		dprint("%s: rx/tx state botch\n", dnam(d));	/* some wd 2tb drives */
		srsirq(d);
		ewake |= Done | Noverdict | Cmdreset;
	}
	ilock(d);
	if(s0 != s1 && d->state == s0)
		d->state = s1;
	iunlock(d);
	if(ewake && (x->cflag&(Done|Active)) == Active){
		unsetci(d);
		x->cflag |= ewake;
		wakeup(x);
		dprint("%s: ewake %.8ux\n", dnam(d), ewake);
	}else if(0)
		dprint("%s: !ewake %.8ux\n", dnam(d), ewake);
	last = cause;
}

static int
satareset(Drive *d)
{
	ilock(d->ctlr);
	unsetci(d);
	iunlock(d->ctlr);
	if(gettask(d) & (ASdrq|ASbsy))
		return -1;
	if(settxmode(d, d->udma) != 0)
		return -1;
	return 0;
}

static int
msriopkt(SDreq *r, Drive *d)
{
	int n, count, t, max, flag, task;
	uchar *cmd;

	cmd = r->cmd;
	aprint("%.2ux %.2ux %c %d %p\n", cmd[0], cmd[2], "rw"[r->write],
		r->dlen, r->data);
	r->rlen = 0;
	count = r->dlen;
	max = 65536;

	for(t = r->timeout; setreqto(r, t) != -1;){
		n = count;
		if(n > max)
			n = max;
		if(lockready(d, t) == -1)
			return SDeio;
		flag = buildpkt(d, r, r->data, n);
		task = gettask(d);
		if(flag & Cmdreset && satareset(d) == -1)
			setstate(d, Dreset);
		qunlock(d);
		if(flag & Noverdict){
			if(flag & Creset)
				setstate(d, Dreset);
			print("%s: retry\n", dnam(d));
			continue;
		}
		if(flag & Error){
			if((task & Eidnf) == 0)
				print("%s: i/o error %ux\n", dnam(d), task);
			return r->status = SDcheck;
		}
		return r->status = SDok;
	}
	print("%s: bad disk\n", dnam(d));
	return r->status = SDcheck;
}

static void
sasreset(SDreq *r, Drive *d)
{
	uint key;

	dprint("%s: sasreset\n", dnam(d));
	ilock(d->ctlr);
	unsetci(d);
	iunlock(d->ctlr);
	tur(r->unit, r->timeout, &key);
}

static int
msriosas(SDreq *r, Drive *d)
{
	int t, flag;

	r->status = ~0;
	for(t = r->timeout; setreqto(r, t) != -1;){
		if(lockready(d, t) == -1)
			return SDeio;
		flag = buildsas(d, r, r->data, r->dlen);
		if(flag & Cmdreset)
			sasreset(r, d);	/* botch; should bounce */
		qunlock(d);
		if(flag & Rate)
			break;
		if(flag & Noverdict){
			if(flag & Creset)
				setstate(d, Dreset);
			if(flag & Timeout){
				dprint("%s: timeout .. retry\n", dnam(d));
				continue;
			}
			if((r->flags & SDvalidsense) == 0)
				setreqsense(r, 0x020401);
			break;
		}
		if(flag & Error){
			print("%s: i/o error\n", dnam(d));
			break;
		}
		r->rlen = r->dlen;	/* fishy */
		return r->status;		/* set in sasrspck */
	}
	if(r->status == ~0)
		r->status = SDtimeout;
	if(r->status == SDcheck && r->sense[0] == 0)
		setreqsense(r, r->write? 0x030c00: 0x031100);
	return r->status;
}

static int
msrio(SDreq *r)
{
	Ctlr *c;
	Drive *d;
	SDunit *u;

	u = r->unit;
	c = u->dev->ctlr;
	d = c->drive + u->subno;
	if(r->timeout == 0)
		r->timeout = totk(Ms2tk(600*1000));
	if(d->feat & Datapi)
		return msriopkt(r, d);
	if(d->type == Sas)
		return msriosas(r, d);
	if(d->type == Sata)
		return atariosata(u, d, r);
	return sdsetsense(r, SDcheck, 3, 0x04, 0x24);
}

static long
msbio(SDunit *u, int lun, int write, void *data, long count, uvlong lba)
{
	Ctlr *c;
	Drive *d;

	c = u->dev->ctlr;
	d = c->drive + u->subno;
	if(d->feat & Datapi || d->type == Sas)
		return scsibiox(u, d, lun, write, data, count, lba);
	return atabio(u, d, lun, write, data, count, lba);
}

/*
 * §6.1.9.5
 * not clear that this is necessary
 * we should know that it's a d2h from the status.
 * pio returns pio setup fises.  hw bug?
 */
static int
sdr(SDreq *r, Drive *d, int st)
{
	uint i;

	if(i = fisoffset(d, 0/*Rd2h*/))
		memmove(r->cmd, d->ctlr->fis + i, 16);
	else
		memset(r->cmd, 0xff, 16);
	r->status = st;
	return st;
}

/*
 * handle oob requests;
 *    restrict & sanitize commands
 */
static int
fisreqchk(Sfis *f, SDreq *r)
{
	uchar *c;

	if((r->ataproto & Pprotom) == Ppkt)
		return SDnostatus;
	if(r->clen != 16)
		error("bad command length");
	c = r->cmd;
	if(c[0] == 0xf0){
		sigtofis(f, r->cmd);
		return r->status = SDok;
	}
	c[0] = H2dev;
	c[1] = Fiscmd;
	c[7] |= Ataobs;
	return SDnostatus;
}

static int
msataio(SDreq *r)
{
	int flag, task, st;
	Ctlr *c;
	Drive *d;
	SDunit *u;
	int (*build)(Drive*, SDreq*, void*, int);

	u = r->unit;
	c = u->dev->ctlr;
	d = c->drive + u->subno;

	if(r->timeout == 0)
		r->timeout = totk(Ms2tk(600*1000));
	if(d->type != Sata)
		error("not sata");
	if(r->cmd[0] == 0xf1){
		d->state = Dreset;
		return r->status = SDok;
	}
	if((r->status = fisreqchk(d, r)) != SDnostatus)
		return r->status;
	build = buildfis;
	if((r->ataproto & Pprotom) == Ppkt)
		build = buildpkt;
if((int)(r->timeout - Ticks) < 0)print("timeout is %ldms; %p\n", TK2MS(r->timeout - Ticks), getcallerpc(&r));
	if(lockready(d, r->timeout) == -1){
		r->status = SDretry;
		return SDok;
	}
	flag = buildfis(d, r, r->data, r->dlen);
	task = gettask(d);
	if(flag & Cmdreset && satareset(d) == -1)
		setstate(d, Dreset);
	qunlock(d);
	st = SDok;
	if(flag & Noverdict){
		st = SDretry;
		if(flag & (Timeout | Creset))
			setstate(d, Dreset);
		else if(task & ASerr){
			/* assume bad cmd */
			st = SDeio;
		}
		print("%s: ata retry\n", dnam(d));
	}
	if(flag & Error){
		print("%s: i/o error %.8ux\n", dnam(d), task);
		st = SDeio;
	}
	if(build != buildpkt)
		r->rlen = r->dlen;
	return sdr(r, d, st);
}

void
dosrsirq(Drive *d)
{
	Cmd *x;

	srsirq(d);
	x = d->cmd;
	if((x->cflag & (Done|Active)) == Active){
		x->cflag |= Done | Noverdict | Cmdreset;
		wakeup(x);
		dprint("%s: ewake %.8ux [srs]\n", dnam(d), x->cflag);
	}else
		dprint("%s: !ewake %.8ux [srs]\n", dnam(d), x->cflag);
}

static uvlong irqtracker;
static uvlong spurious;

static void
msinterrupt(Ureg*, void *a)
{
	Ctlr *c;
	uint i, u, v;
	static uint cnt;

	c = a;
	ilock(c);
	u = c->reg[Cis];
	c->reg[Cis] = u & ~Iclr;
	if(u == 0){
spurious++;
		iunlock(c);
		return;
	}
irqtracker++;
	if(u != Cdone && cnt++ < 15)
		dprint("%s: irq %.8ux\n", c->sdev->name, u);
	if(u & Prderr)
		dprint("sd%s: cprderr: %d %.8ux\n", cnam(c), ++c->prderr, u);
	if(u & Srsirq){
		v = c->reg[Csis];
		c->reg[Csis] = v;
		dprint("sd%s: srs %.8ux\n", cnam(c), v);
		for(i = 0; i < c->ndrive; i++)
			if(v & 1<<i)
				dosrsirq(c->drive + i);
	}
	for(i = 0; i < c->ndrive; i++)
		if(u & (1<<i)*(Portirq|Portstop))
			updatedrive(c->drive + i);
	if(u & Cdone)
		updatedone(c);
	iunlock(c);
}

static int
newsatadrive(Drive *d)
{
	uint task;

	task = gettask(d);
	if((task & 0xffff) == 0x80)
		return SDretry;
	setfissig(d, getsig(d));
	return ataonline(d->unit, d);
}

static int
setconninfo(Drive *d)
{
	uint i;
	uvlong sa;
	Ctlr *c;

	i = d->driveno;
	c = d->ctlr;

	sa = pcread64(c, i, Pawwn);
	if(sa == 0 || sa == ~0ull){	/* driver bug */
		dprint("%s: driver bug: sas addr %.16llux\n", dnam(d), sa);
		return -1;
	}
	putbe(d->tsasaddr, sa, 8);
	putbe(d->ict, i<<1, 2);
	return 0;
}

static int
newsasdrive(Drive *d)
{
	memset(d->cmd->rsp, 0, sizeof d->cmd->rsp);
	if(setconninfo(d) == -1)
		return SDeio;
	d->wwn = getbe(d->tsasaddr, 8);
	return scsionlinex(d->unit, d);
}

static int
newdrive(Drive *d)
{
	char *t;
	int r;

	memset(&d->Sfis, 0, sizeof d->Sfis);
	memset(&d->Cfis, 0, sizeof d->Cfis);
	d->atamaxxfr = ~0;
	d->maxspd = Spd30;
	switch(d->type){
	case Sata:
		r = newsatadrive(d);
		break;
	case Sas:
		r = newsasdrive(d);
		break;
	default:
		print("%s: bug: martian drive %d\n", dnam(d), d->type);
		return -1;
	}
	qlock(d);
	t = type[d->type];
	switch(r){
	case SDok:
		pronline(d->unit, d);
		setstate(d, Dready);
		break;
	case SDeio:
		idprint("%s: %s can't be initialized\n", dnam(d), t);
		setstate(d, Derror);
	case SDretry:
		break;
	}
	qunlock(d);
	return r;
}

static void
statechange(Drive *d)
{
	switch(d->state){
	case Dmissing:
	case Dnull:
	case Doffline:
		d->drivechange = 1;
		d->unit->sectors = 0;
		break;
	case Dready:
		d->wait = 0;
		break;
	}
}

static void
checkdrive(Drive *d, int i)
{
	uint s;

	if(d->unit == nil)
		return;
	ilock(d);
	s = sstatus(d);
	d->wait++;
	if(s & Sphyrdy)
		d->lastseen = Ticks;
	if(s != olds[i]){
		dprint("%s: status: %.6ux -> %.6ux: %s\n",
			dnam(d), olds[i], s, dstate(d->state));
		olds[i] = s;
		statechange(d);
	}
	switch(d->state){
	case Dnull:
	case Dmissing:
		if(d->type != 0 && s & Sphyrdy)
			d->state = Dnew;
		break;
	case Dnopower:
		phyreset(d);	/* spinup */
		break;
	case Dnew:
		iunlock(d);
		newdrive(d);
		ilock(d);
		break;
	case Dready:
		d->wait = 0;
		break;
	case Derror:
		d->wait = 0;
		d->state = Dreset;
	case Dreset:
		if(d->wait % 40 != 0)
			break;
		reset(d);
		break;
	case Doffline:
	case Dportreset:
		break;
	}
	iunlock(d);
}

static void
mskproc(void*)
{
	int i;

	for(;;){
		tsleep(&up->sleep, return0, 0, Nms);
		for(i = 0; i < nmsdrive; i++)
			checkdrive(msdrive[i], i);
	}
}

static void
ledcfg(Ctlr *c, int port, uint cfg)
{
	uint u, r, s;

	r = Drivectl + (port>>2)*Gpiooff;
	s = 15 - port & 3;
	s *= 8;
	u = gpread(c, r);
	u &= ~(0xff << s);
	u |= cfg<<s;
	gpwrite(c, r, u);
}

static uchar ses2ledstd[Ibpilast] = {
[Ibpinone]	Lhigh*Aled,
[Ibpinormal]	Lsof*Aled | Llow*Locled | Llow*Errled,
[Ibpirebuild]	Lsof*Aled | Llow*Locled | Llow*Errled,
[Ibpilocate]	Lsof*Aled | Lblinka*Locled | Llow*Errled,
[Ibpispare]	Lsof*Aled | Llow*Locled| Lblinka*Errled,
[Ibpipfa]		Lsof*Aled | Lblinkb*Locled | Llow*Errled,
[Ibpifail]		Lsof*Aled | Llow*Locled | Lhigh*Errled,
[Ibpicritarray]	Lsof*Aled,
[Ibpifailarray]	Lsof*Aled,
};

static uchar ses2led[Ibpilast] = {
[Ibpinone]	Lhigh*Aled,
[Ibpinormal]	Lsof*Aled | Llow*Locled | Llow*Errled,
[Ibpirebuild]	Lsof*Aled | Lblinkaneg*Locled | Llow*Errled,
[Ibpilocate]	Lsof*Aled | Lhigh*Locled | Llow*Errled,
[Ibpispare]	Lsof*Aled | Lblinka*Locled| Llow*Errled,
[Ibpipfa]		Lsof*Aled | Lblinkb*Locled | Llow*Errled,
[Ibpifail]		Lsof*Aled | Llow*Locled | Lhigh*Errled,
[Ibpicritarray]	Lsof*Aled,
[Ibpifailarray]	Lsof*Aled,
};

static void
setupled(Ctlr *c)
{
	int i, l, blen;

	pcicfgw32(c->pci, Gpio, pcicfgr32(c->pci, Gpio) | 1<<7);
	/*
	 * configure a for 4hz (1/8s on and 1/8s off)
	 * configure b for 1hz (2/8s on and 6/8s off)
	 */
	l = 3 + c->ndrive >> 2;
	blen = 3*24 - 1;
	for(i = 0; i < l*Gpiooff; i += Gpiooff){
		gpwrite(c, Sgconf0 + i, blen*Autolen | Blinkben | Blinkaen | Sgpioen);
		gpwrite(c, Sgconf1 + i, 1*Bhi | 1*Blo | 1*Ahi | 7*Alo);
		gpwrite(c, Sgconf3 + i, 7<<20 | Sdoutauto);
	}
}

static long
odinledr(SDunit *u, Chan *ch, void *a, long n, vlong off)
{
	Ctlr *c;
	Drive *d;

	c = u->dev->ctlr;
	d = c->drive + u->subno;
	return ledr(d, ch, a, n, off);
}

static void
setled(Drive *d)
{
	uint bits;

	bits = ses2led[d->led];
	if(d->ledbits != bits){
		ledcfg(d->ctlr, d->driveno, bits);
		d->ledbits = bits;
	}
}

static long
odinledw(SDunit *u, Chan *ch, void *a, long n, vlong off)
{
	Ctlr *c;
	Drive *d;

	c = u->dev->ctlr;
	d = c->drive + u->subno;
	ledw(d, ch, a, n, off);
	setled(d);
	return n;
}

static int
msenable(SDev *s)
{
	char buf[32];
	int en, i;
	Ctlr *c;
	static int once;

	c = s->ctlr;
	ilock(c);
	en = 0;
	if(!c->enabled){
		if(once++ == 0)
			kproc("odin", mskproc, 0);
		c->enabled = 1;
		en = 1;
	}
	iunlock(c);

	if(en){
		pcisetbme(c->pci);
		snprint(buf, sizeof buf, "%s (%s)", s->name, s->ifc->name);
		intrenable(c->pci->intl, msinterrupt, c, c->pci->tbdf, buf);
		c->reg[Gctl] |= Intenable;
		for(i = 0; i < c->ndrive; i++)
			c->reg[pis[i] + 1] =
				Sync | Phyerr | Stperr | Crcerr |
				Linkrx | Martianfis | Anot | Bist | Sigrx |
				Phyunrdy | Martiantag | Bnot | Comw |
				Portsel | Hreset | Phyidto | Phyidok |
				Hresetok | Phyrdy;
	}
	return 1;
}

static int
msdisable(SDev *s)
{
	Ctlr *c;

	c = s->ctlr;
	ilock(c);
//	disable(c->hba);
	c->reg[Gctl] &= ~Intenable;
	intrdisable(c->vector);
	c->enabled = 0;
	iunlock(c);
	return 1;
}

static int
msonline(SDunit *u)
{
	int r;
	Ctlr *c;
	Drive *d;

	c = u->dev->ctlr;
	d = c->drive + u->subno;
	r = 0;

	if(d->feat & Datapi){
		if(!d->drivechange)
			return r;
		r = scsionlinex(u, d) == SDok;
		if(r > 0)
			d->drivechange = 0;
		return r;
	}
	ilock(d);
	if(d->drivechange){
		r = 2;
		d->drivechange = 0;
		u->sectors = d->sectors;
		u->secsize = d->secsize;
	} else if(d->state == Dready)
		r = 1;
	iunlock(d);
	return r;
}

static void
verifychk(Drive *d)
{
	int w;

	if(!up)
		checkdrive(d, d->driveno);
	for(w = 0; w < 12000; w += 210){
		if(d->state == Dready)
			break;
		if(w > 2000 && d->state != Dnew)
			break;
		if((sstatus(d) & Sphyrdy) == 0)
			break;
		if(!up)
			checkdrive(d, d->driveno);
		esleep(210);
	}
}

static int
msverify(SDunit *u)
{
	int chk;
	Ctlr *c;
	Drive *d;

	c = u->dev->ctlr;
	d = c->drive + u->subno;
	ilock(c);
	ilock(d);
	chk = 0;
	if(d->unit == nil){
		d->unit = u;
		sdaddfile(u, "led", 0644, eve, odinledr, odinledw);
		chk = 1;
	}
	iunlock(d);
	iunlock(c);

	/*
	 * since devsd doesn't know much about hot-plug drives,
	 * we need to give detected drives a chance.
	 */
	if(chk){
		if((sstatus(d) & Sphyrdy) == 0)
			reset(d);
		verifychk(d);
	}
	return 1;
}

static uint*
map(Pcidev *p, int bar)
{
	uintptr io;

	io = p->mem[bar].bar & ~(uintmem)0xf;
	return (uint*)vmap(io, p->mem[bar].size);
}

/* §5.1.3 */
static void
initmem(Ctlr *c)
{
	c->fis = smalloc(Fis0 + 0x100*16);	/* §6.1.9.3 */
	c->reg[Fisbase + 0] = Pciwaddrl(c->fis);
	c->reg[Fisbase + 1] = Pciwaddrh(c->fis);
	c->reg[Cqbase + 0] = Pciwaddrl(c->cq);
	c->reg[Cqbase + 1] = Pciwaddrh(c->cq);
	c->reg[Cqcfg] = Cqen | Noattn | Nqueue;
	c->reg[Dqbase + 0] = Pciwaddrl(c->dq);
	c->reg[Dqbase + 1] = Pciwaddrh(c->dq);
	c->reg[Dqcfg] = Dqen | Nqueue;
	c->cl = smalloc((Nqueue+1)*sizeof *c->cl);
	c->reg[Clbase + 0] = Pciwaddrl(c->cl);
	c->reg[Clbase + 1] = Pciwaddrh(c->cl);
	c->cmdtab = smalloc(Nctlrdrv*sizeof *c->cmdtab);
}

static void
errata(Ctlr *c)
{
	int i;

	sswrite(c, 0, Sasctl0, ssread(c, 0, Sasctl0) & ~0xffff | 0xffff);	/* nexus loss bug */
	sswrite(c, 0, Pwdtimer, 0x7fffff);				/* watchdog bug */
//	sswrite(c, 0, Phytimer, ssread(c, 0, Phytimer) & ~0x200 | 0x400);	/* magic timer bug */

	/* supposed workaround for segate disks; 0x1b8 appears misued */
//	sswrite(c, 0, 0x1b8, ssread(c, 0, 0x1b8) & 0xffff | 0xfa0000);
//	sswrite(c, 0, Phytimer, ssread(c, 0, Phytimer) & ~(7<<29) | 2<<29);

	/* adjust phy voltage.  magic undocumented registers. */
	for(i = 0; i < 4; i++)
		vswrite(c, i, 8, 0x2f0);
}

/* §5.1.2 */
static void
startup(Ctlr *c)
{
	c->reg[Gctl] |= Reset;
	while(c->reg[Gctl] & Reset)
		;
	initmem(c);
	c->reg[Cie] = Swirq1 | 0xff*Portstop | 0xff*Portirq | Srsirq | Issstop | Cdone;
	c->reg[Portcfg0] = Rmask*Regen | Dataunke | Rsple | Framele;
	c->reg[Portcfg1] = Rmask*Regen | 0xff*Xmten | Cmdirq*0 | Fisen | Resetiss | Issueen;
	c->reg[Csie] = ~0;
	errata(c);
	c->reg[Coal] = 0x10002;		/* enable, coal two commands */
	c->reg[Coalto] = 0x10000 | 50;	/* set us, 50µs max */
}

static void
forcetype(Ctlr*)
{
	/*
	 * if we want to force sas/sata, here's where to do it.
	 */
}

static void
setupcmd(Drive *d)
{
	int i;
	Ctlr *c;
	Cmd *cmd;
	Cmdh *h;

	i = d->driveno;
	c = d->ctlr;
	d->cmd = c->cmdtab + i;
	d->cmd->cmdh = c->cl + i;
	cmd = d->cmd;
	h = cmd->cmdh;

	/* talk to Cfisx */
	d->oaf = d->cmd->oaf;

	/* prep the precomputable bits in the cmd hdr §6.1.4 */
	putle(h->ctab, Pciw64(&cmd->Ctab), sizeof h->ctab);
	putle(h->oaf, Pciw64(&cmd->Oaf), sizeof h->oaf);
	putle(h->statb, Pciw64(&cmd->Statb), sizeof h->statb);
	putle(h->prd, Pciw64(&cmd->Aprdt), sizeof h->prd);

	/* finally, set up the wide-port participating bit */
	pcwrite(c, i, Pwidecfg, 1<<i);
}

static SDev*
mspnp(void)
{
	int i, nunit;
	Ctlr *c;
	Drive *d;
	Pcidev *p;
	SDev *s;
	static int done;

	if(done++)
		return nil;
	for(p = nil; (p = pcimatch(p, 0x11ab, 0x6485)) != nil; ){
		if(nmsctlr == Nctlr){
			print("sdodin: %T: too many controllers\n", p->tbdf);
			continue;
		}
		if(Nctlrdrv * Maxout >= Nqueue - 1){
			print("sdodin: too few cq slots\n");
			continue;
		}
		c = msctlr + nmsctlr;
		s = sdevs + nmsctlr;
		memset(c, 0, sizeof *c);
		memset(s, 0, sizeof *s);
		c->dq = (uint*)ROUNDUP((uintptr)c->mem, Cacheline);
		c->cq = (uint*)ROUNDUP((uintptr)(c->dq+Nqueue), Cacheline);
		if((c->reg = map(p, Mebar)) == 0){
			print("sdodin: bar %#p in use\n", c->reg);
			continue;
		}
		nunit = p->did>>4 & 0xf;
		s->ifc = &sdodinifc;
		s->idno = 'a' + nmsctlr;
		s->ctlr = c;
		c->sdev = s;
		c->pci = p;
		c->ndrive = s->nunit = nunit;
		i = pcicfgr32(p, Dctl) & ~(7<<12);
		pcicfgw32(p, Dctl, i | 4<<12);
		sdadddevs(s);
		print("#S/%s: odin ii sata/sas with %d ports\n", s->name, nunit);
		startup(c);
		forcetype(c);
		setupled(c);
		for(i = 0; i < nunit; i++){
			d = c->drive + i;
			d->driveno = i;
			d->sectors = 0;
			d->ctlr = c;
			d->tler = 5000;
			d->nled = 2;	/* how to know? */
			d->led = Ibpinormal;
			setled(d);
			setupcmd(d);
			snprint(d->name, sizeof d->name, "%s%d.%d",
				s->ifc->name, nmsctlr, i);
			msdrive[nmsdrive + i] = d;
			c->reg[pis[i] + 1] =
				Sync | Phyerr | Stperr | Crcerr |
				Linkrx | Martianfis | Anot | Bist | Sigrx |
				Phyunrdy | Martiantag | Bnot | Comw |
				Portsel | Hreset | Phyidto | Phyidok |
				Hresetok | Phyrdy;
		}
		nmsdrive += nunit;
		nmsctlr++;
	}
	return nil;
}

static char*
rctldebug(char *p, char *e, Ctlr *c, Drive *d)
{
	int i;

	p = seprint(p, e, "irq	%lld %lld\n", irqtracker, spurious);

	i = d->driveno;
	p = seprint(p, e, "sstatus\t%.8ux\n", sstatus(d));
//	p = seprint(p, e, "cis\t%.8ux %.8ux\n", c->reg[Cis], c->reg[Cie]);
//	p = seprint(p, e, "gis\t%.8ux\n", c->reg[Gis]);
	p = seprint(p, e, "pis\t%.8ux %.8ux\n", c->reg[pis[i]], c->reg[pis[i] + 1]);
	p = seprint(p, e, "sis\t%.8ux\n", c->reg[Csis]);
	p = seprint(p, e, "cqwp\t%.8ux\n", c->cq[0]);
	p = seprint(p, e, "cerror\t%.8ux %.8ux\n", *(uint*)d->cmd->error, *(uint*)(d->cmd->error+4));
	p = seprint(p, e, "task\t%.8ux\n", gettask(d));
	p = seprint(p, e, "ptype\t%.8ux\n", c->reg[Ptype]);
	p = seprint(p, e, "satactl\t%.8ux\n", pcread(c, i, Psatactl));	/* appears worthless */
	p = seprint(p, e, "info	%.8ux %.8ux\n", pcread(c, i, Pinfo), pcread(c, i, Painfo));
	p = seprint(p, e, "physts	%.8ux\n", pcread(c, i, Pphysts));
	p = seprint(p, e, "widecfg	%.8ux\n", pcread(c, i, Pwidecfg));
	p = seprint(p, e, "wwn	%.16llux %.8ux\n", pcread64(c, i, Pwwn), pcread(c, i, Pwwn + 8));
	p = seprint(p, e, "awwn	%.16llux\n", pcread64(c, i, Pawwn));
	p = seprint(p, e, "sasid	%.16llux\n", pcread64(c, i, Paddr));
	p = seprint(p, e, "phyerr: %.8ux\n", pcread(c, i, Perr));
	p = seprint(p, e, "phycrcerr: %.8ux\n", pcread(c, i, Pcrcerr));
	return p;
}

static int
msrctl(SDunit *u, char *p, int l)
{
	char *e, *op;
	Ctlr *c;
	Drive *d;

	if((c = u->dev->ctlr) == nil)
		return 0;
	d = c->drive + u->subno;
	e = p + l;
	op = p;
	p = seprint(p, e, "state\t%s\n", dstate(d->state));
	p = seprint(p, e, "type\t%s", type[d->type]);
	if(d->type == Sata)
		p = seprint(p, e, " sig %.8ux", getsig(d));
	p = seprint(p, e, "\n");
	if(d->state == Dready)
		p = sfisxrdctl(d, p, e);
	p = rctldebug(p, e, c, d);
	p = seprint(p, e, "geometry %llud %lud\n", d->sectors, u->secsize);
	return p - op;
}

static void
forcestate(Drive *d, char *state)
{
	int i;

	for(i = 1; i < nelem(diskstates); i++)
		if(strcmp(state, diskstates[i]) == 0)
			break;
	if(i == nelem(diskstates))
		error(Ebadctl);
	ilock(d);
	d->state = 1 << i - 1;
	statechange(d);
	iunlock(d);
}

static int
mswctl(SDunit *u, Cmdbuf *cmd)
{
	char **f;
	Ctlr *c;
	Drive *d;

	c = u->dev->ctlr;
	d = c->drive + u->subno;
	f = cmd->f;
	if(strcmp(f[0], "state") == 0)
		forcestate(d, f[1]? f[1]: "null");
	else
		cmderror(cmd, Ebadctl);
	return 0;
}

static int
mswtopctl(SDev*, Cmdbuf *cmd)
{
	char **f;
	int *v;

	f = cmd->f;
	v = 0;
	if(strcmp(f[0], "debug") == 0)
		v = &debug;
	else if(strcmp(f[0], "idprint") == 0)
		v = &idebug;
	else if(strcmp(f[0], "aprint") == 0)
		v = &adebug;
	else
		cmderror(cmd, Ebadctl);
	if(cmd->nf == 1)
		*v ^= 1;
	else if(cmd->nf == 2)
		*v = strcmp(f[1], "on") == 0;
	else
		cmderror(cmd, Ebadarg);
	return 0;
}

SDifc sdodinifc = {
	"odin",
	mspnp,
	nil,
	msenable,
	msdisable,
	msverify,
	msonline,
	msrio,
	msrctl,
	mswctl,
	msbio,
	nil,		/* probe */
	nil,		/* clear */
	nil,		/* rtopctl */
	mswtopctl,
	msataio,
};
