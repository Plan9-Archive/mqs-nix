typedef	struct	Hwconf	Hwconf;
typedef	struct	Pciconf	Pciconf;
typedef	struct	Pcidev	Pcidev;
typedef	struct	Vctl	Vctl;
typedef	struct	Vkey	Vkey;
typedef	struct	Vtime	Vtime;

enum {
	VectorNMI	= 2,		/* non-maskable interrupt */
	VectorBPT	= 3,		/* breakpoint */
	VectorUD	= 6,		/* invalid opcode exception */
	VectorCNA	= 7,		/* coprocessor not available */
	Vector2F	= 8,		/* double fault */
	VectorCSO	= 9,		/* coprocessor segment overrun */
	VectorPF	= 14,		/* page fault */
	Vector15	= 15,		/* reserved */
	VectorCERR	= 16,		/* coprocessor error */
	VectorSIMD	= 19,		/* SIMD error */

	VectorPIC	= 32,		/* external i8259 interrupts */
	IrqCLOCK	= 0,
	IrqKBD		= 1,
	IrqUART1	= 3,
	IrqUART0	= 4,
	IrqPCMCIA	= 5,
	IrqFLOPPY	= 6,
	IrqLPT		= 7,
	IrqIRQ7		= 7,
	IrqAUX		= 12,		/* PS/2 port */
	IrqIRQ13	= 13,		/* coprocessor on 386 */
	IrqATA0		= 14,
	IrqATA1		= 15,
	MaxIrqPIC	= 15,

	VectorLAPIC	= VectorPIC+16,	/* local APIC interrupts */
	IrqLINT0	= VectorLAPIC+0,
	IrqLINT1	= VectorLAPIC+1,
	IrqTIMER	= VectorLAPIC+2,
	IrqERROR	= VectorLAPIC+3,
	IrqPCINT	= VectorLAPIC+4,
	IrqSPURIOUS	= VectorLAPIC+15,
	MaxIrqLAPIC	= VectorLAPIC+15,

	VectorSYSCALL	= 64,

	VectorAPIC	= 65,		/* external APIC interrupts */
	MaxVectorAPIC	= 255,
};

enum {
	IdtPIC		= 32,			/* external i8259 interrupts */

	IdtLINT0	= 48,			/* local APIC interrupts */
	IdtLINT1	= 49,
	IdtTIMER	= 50,
	IdtERROR	= 51,
	IdtPCINT	= 52,

	IdtIPI		= 62,
	IdtSPURIOUS	= 63,

	IdtSYSCALL	= 64,

	IdtIOAPIC	= 65,			/* external APIC interrupts */

	IdtMAX		= 255,
};

enum {
	Vmsix		= 1<<0,			/* allow msi-x allocation (unfortunately this changes handlers */
};

struct Vkey {
	int	tbdf;			/* pci: ioapic or msi sources */
	int	irq;			/* 8259-emulating sources */
};

struct Vtime {
	uvlong	count;
	uvlong	cycles;
};

struct Vctl {
	Vctl*	next;			/* handlers on this vector */

	uchar	isintr;			/* interrupt or fault/trap */
	uchar	flag;
	int	affinity;			/* processor affinity (-1 for none) */

	Vkey;				/* source-specific key; tbdf for pci */
	void	(*f)(Ureg*, void*);	/* handler to call */
	void*	a;			/* argument to call it with */
	char	name[KNAMELEN];		/* of driver */
	char	*type;

	int	(*isr)(int);		/* get isr bit for this irq */
	int	(*eoi)(int);		/* eoi */
	int	(*mask)(Vkey*, int);	/* interrupt enable returns masked vector */
	int	vno;

	Vtime;
};

enum {
	BusCBUS		= 0,		/* Corollary CBUS */
	BusCBUSII,			/* Corollary CBUS II */
	BusEISA,			/* Extended ISA */
	BusFUTURE,			/* IEEE Futurebus */
	BusINTERN,			/* Internal bus */
	BusISA,				/* Industry Standard Architecture */
	BusMBI,				/* Multibus I */
	BusMBII,			/* Multibus II */
	BusMCA,				/* Micro Channel Architecture */
	BusMPI,				/* MPI */
	BusMPSA,			/* MPSA */
	BusNUBUS,			/* Apple Macintosh NuBus */
	BusPCI,				/* Peripheral Component Interconnect */
	BusPCMCIA,			/* PC Memory Card International Association */
	BusTC,				/* DEC TurboChannel */
	BusVL,				/* VESA Local bus */
	BusVME,				/* VMEbus */
	BusXPRESS,			/* Express System Bus */
};

#define MKBUS(t,b,d,f)	(((t)<<24)|(((b)&0xFF)<<16)|(((d)&0x1F)<<11)|(((f)&0x07)<<8))
#define BUSFNO(tbdf)	(((tbdf)>>8)&0x07)
#define BUSDNO(tbdf)	(((tbdf)>>11)&0x1F)
#define BUSBNO(tbdf)	(((tbdf)>>16)&0xFF)
#define BUSTYPE(tbdf)	((tbdf)>>24)
#define BUSBDF(tbdf)	((tbdf)&0x00FFFF00)
#define BUSUNKNOWN	(-1)

/*
 * PCI support code.
 */
enum {					/* type 0 and type 1 pre-defined header */
	PciVID		= 0x00,		/* vendor ID */
	PciDID		= 0x02,		/* device ID */
	PciPCR		= 0x04,		/* command */
	PciPSR		= 0x06,		/* status */
	PciRID		= 0x08,		/* revision ID */
	PciCCRp		= 0x09,		/* programming interface class code */
	PciCCRu		= 0x0A,		/* sub-class code */
	PciCCRb		= 0x0B,		/* base class code */
	PciCLS		= 0x0C,		/* cache line size */
	PciLTR		= 0x0D,		/* latency timer */
	PciHDT		= 0x0E,		/* header type */
	PciBST		= 0x0F,		/* BIST */

	PciBAR0		= 0x10,		/* base address */
	PciBAR1		= 0x14,

	PciCP		= 0x34,		/* capabilities pointer */

	PciINTL		= 0x3C,		/* interrupt line */
	PciINTP		= 0x3D,		/* interrupt pin */
};

enum {					/* type 0 pre-defined header */
	PciCIS		= 0x28,		/* cardbus CIS pointer */
	PciSVID		= 0x2C,		/* subsystem vendor ID */
	PciSID		= 0x2E,		/* cardbus CIS pointer */
	PciEBAR0	= 0x30,		/* expansion ROM base address */
	PciMGNT		= 0x3E,		/* burst period length */
	PciMLT		= 0x3F,		/* maximum latency between bursts */
};

enum {					/* type 1 pre-defined header */
	PciPBN		= 0x18,		/* primary bus number */
	PciSBN		= 0x19,		/* secondary bus number */
	PciUBN		= 0x1A,		/* subordinate bus number */
	PciSLTR		= 0x1B,		/* secondary latency timer */
	PciIBR		= 0x1C,		/* I/O base */
	PciILR		= 0x1D,		/* I/O limit */
	PciSPSR		= 0x1E,		/* secondary status */
	PciMBR		= 0x20,		/* memory base */
	PciMLR		= 0x22,		/* memory limit */
	PciPMBR		= 0x24,		/* prefetchable memory base */
	PciPMLR		= 0x26,		/* prefetchable memory limit */
	PciPUBR		= 0x28,		/* prefetchable base upper 32 bits */
	PciPULR		= 0x2C,		/* prefetchable limit upper 32 bits */
	PciIUBR		= 0x30,		/* I/O base upper 16 bits */
	PciIULR		= 0x32,		/* I/O limit upper 16 bits */
	PciEBAR1	= 0x28,		/* expansion ROM base address */
	PciBCR		= 0x3E,		/* bridge control register */
};

/* capabilities */
enum {
	PciCapPMG	= 0x01,		/* power management */
	PciCapAGP	= 0x02,
	PciCapVPD	= 0x03,		/* vital product data */
	PciCapSID	= 0x04,		/* slot id */
	PciCapMSI	= 0x05,
	PciCapCHS	= 0x06,		/* compact pci hot swap */
	PciCapPCIX	= 0x07,
	PciCapHTC	= 0x08,		/* hypertransport irq conf */
	PciCapVND	= 0x09,		/* vendor specific information */
	PciCapPCIe	= 0x10,
	PciCapMSIX	= 0x11,
	PciCapSATA	= 0x12,
	PciCapHSW	= 0x0c,		/* hot swap */
};

struct Pcidev {
	int	tbdf;			/* type+bus+device+function */
	ushort	vid;			/* vendor ID */
	ushort	did;			/* device ID */

	ushort	pcr;

	uchar	rid;
	uchar	ccrp;
	uchar	ccru;
	uchar	ccrb;
	uchar	cls;
	uchar	ltr;
	uchar	intl;			/* interrupt line */

	struct {
		uintmem	bar;		/* base address */
		int	size;
	} mem[6];

	Pcidev*	list;
	Pcidev*	link;			/* next device on this bno */
	Pcidev*	bridge;			/* down a bus */
};

struct Pciconf {
	char	*type;
	uintmem	port;
	uintmem	mem;

	int	irq;
	int	tbdf;

	int	nopt;
	char	optbuf[128];
	char	*opt[8];
};

struct Hwconf {
	Pciconf;
};

void	archpciinit(void);
int	pcicap(Pcidev*, int);
int	pcicfgr16(Pcidev*, int);
uint	pcicfgr32(Pcidev*, int);
int	pcicfgr8(Pcidev*, int);
void	pcicfgw16(Pcidev*, int, int);
void	pcicfgw32(Pcidev*, int, int);
void	pcicfgw8(Pcidev*, int, int);
void	pciclrbme(Pcidev*);
void	pciclriome(Pcidev*);
void	pciclrmwi(Pcidev*);
int	pciconfig(char*, int, Pciconf*);
int	pcigetpms(Pcidev*);
void	pcihinv(Pcidev*);
Pcidev*	pcimatch(Pcidev*, int, int);
Pcidev*	pcimatchtbdf(int);
void	pcireset(void);
void	pcisetbme(Pcidev*);
void	pcisetioe(Pcidev*);
void	pcisetmwi(Pcidev*);
int	pcisetpms(Pcidev*, int);
int	strtotbdf(char*, char**, int);

#define PCIWINDOW	0
#define PCIWADDR(va)	(PADDR(va)+PCIWINDOW)
#define Pciwaddr(va)	PCIWADDR(va)
#define Pciwaddrl(va)	((u32int)PCIWADDR(va))
#define Pciwaddrh(va)	((u32int)(PCIWADDR(va)>>32))

#pragma	varargck	type	"T"	int
