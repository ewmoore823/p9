/*
 * PCI support code.
 * Needs a massive rewrite.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"

#define DBG(...)

enum
{					/* configuration mechanism #1 */
	PciADDR		= 0xCF8,	/* CONFIG_ADDRESS */
	PciDATA		= 0xCFC,	/* CONFIG_DATA */

					/* configuration mechanism #2 */
	PciCSE		= 0xCF8,	/* configuration space enable */
	PciFORWARD	= 0xCFA,	/* which bus */

	MaxFNO		= 7,
	MaxUBN		= 255,
};

enum
{					/* command register */
	IOen		= (1<<0),
	MEMen		= (1<<1),
	MASen		= (1<<2),
	MemWrInv	= (1<<4),
	PErrEn		= (1<<6),
	SErrEn		= (1<<8),
};

static Lock pcicfglock;
static Lock pcicfginitlock;
static int pcicfgmode = -1;
static int pcimaxbno = 255;
static int pcimaxdno;
static Pcidev* pciroot;
static Pcidev* pcilist;
static Pcidev* pcitail;
static int nobios, nopcirouting;
static BIOS32si* pcibiossi;

static int pcicfgrw8raw(int, int, int, int);
static int pcicfgrw16raw(int, int, int, int);
static int pcicfgrw32raw(int, int, int, int);

static int (*pcicfgrw8)(int, int, int, int) = pcicfgrw8raw;
static int (*pcicfgrw16)(int, int, int, int) = pcicfgrw16raw;
static int (*pcicfgrw32)(int, int, int, int) = pcicfgrw32raw;

static char* bustypes[] = {
	"CBUSI",
	"CBUSII",
	"EISA",
	"FUTURE",
	"INTERN",
	"ISA",
	"MBI",
	"MBII",
	"MCA",
	"MPI",
	"MPSA",
	"NUBUS",
	"PCI",
	"PCMCIA",
	"TC",
	"VL",
	"VME",
	"XPRESS",
};

static int
strtobus(char *s)
{
	int i;

	for(i = 0; i < nelem(bustypes); i++)
		if(cistrcmp(s, bustypes[i]) == 0)
			return i;
	return BUSUNKNOWN;
}

int
strtotbdf(char *p, char **r, int base)
{
	char buf[12], *f0[4], **f;
	int i, n, bus, t[4];

	if(strchr(p, '.') == nil)
		return strtoul(p, r, base);

	snprint(buf, sizeof buf, "%s", p);
	f = f0;
	n = getfields(buf, f, nelem(f0), 0, ".");
	memset(t, 0, sizeof t);
	t[0] = BusPCI;
	if((bus = strtobus(f[0])) != BUSUNKNOWN){
		t[0] = bus;
		f++;
		n--;
	}
	for(i = 0; i < n; i++)
		t[i+1] = strtoul(f[i], r, base);
	if(r != nil)
		*r = *r-buf + p;
	return MKBUS(t[0], t[1], t[2], t[3]);
}

static int
fmtT(Fmt* fmt)
{
	char *p;
	int l, r;
	uint type, tbdf;

	if((p = malloc(READSTR)) == nil)
		return fmtstrcpy(fmt, "(tbdfconv)");

	switch(fmt->r){
	case 'T':
		tbdf = va_arg(fmt->args, uint);
		type = BUSTYPE(tbdf);
		if(type < nelem(bustypes))
			l = snprint(p, READSTR, bustypes[type]);
		else
			l = snprint(p, READSTR, "%d", type);
		snprint(p+l, READSTR-l, ".%d.%d.%d",
			BUSBNO(tbdf), BUSDNO(tbdf), BUSFNO(tbdf));
		break;

	default:
		snprint(p, READSTR, "(tbdfconv)");
		break;
	}
	r = fmtstrcpy(fmt, p);
	free(p);

	return r;
}

u32int
pcibarsize(Pcidev *p, int rno)
{
	ulong v, size;

	v = pcicfgrw32(p->tbdf, rno, 0, 1);
	pcicfgrw32(p->tbdf, rno, 0xFFFFFFF0, 0);
	size = pcicfgrw32(p->tbdf, rno, 0, 1);
	if(v & 1)
		size |= 0xFFFF0000;
	pcicfgrw32(p->tbdf, rno, v, 0);

	return -(size & ~0x0F);
}

static void
pcibars(Pcidev *p)
{
	int i, o;
	u32int bar;

	for(i = 0; i < nelem(p->mem); i++) {
		o = PciBAR0+4*i;
		p->mem[i].bar = (u32int)pcicfgr32(p, o);
		p->mem[i].size = pcibarsize(p, o);
		if(i&1 || (p->mem[i].bar & 2<<1) == 0)
			continue;
		bar = pcicfgr32(p, o+4);
		if(sizeof(uintmem) >= sizeof(u64int))
			p->mem[i].bar |= (u64int)bar<<32;
		else if(bar != 0){
			print("%T: warning 64-bit bar %d too large\n", p->tbdf, i);
			p->mem[i].bar = 0;
			p->mem[i].size = 0;
		}
		i++;
		p->mem[i].bar = 0;
		p->mem[i].size = 0;
	}
}

static int
pcisizcmp(void *a, void *b)
{
	Pcisiz *aa, *bb;

	aa = a;
	bb = b;
	return aa->siz - bb->siz;
}

static ulong
pcimask(ulong v)
{
	ulong m;

	m = BI2BY*sizeof(v);
	for(m = 1<<(m-1); m != 0; m >>= 1) {
		if(m & v)
			break;
	}

	m--;
	if((v & m) == 0)
		return v;

	v |= m;
	return v+1;
}

static void
pcibusmap(Pcidev *root, ulong *pmema, ulong *pioa, int wrreg)
{
	Pcidev *p;
	int ntb, i, size, rno, hole;
	ulong v, mema, ioa, sioa, smema, base, limit;
	Pcisiz *table, *tptr, *mtb, *itb;

	if(!nobios)
		return;

	ioa = *pioa;
	mema = *pmema;

	DBG("pcibusmap wr=%d %T mem=%luX io=%luX\n",
		wrreg, root->tbdf, mema, ioa);

	ntb = 0;
	for(p = root; p != nil; p = p->link)
		ntb++;

	ntb *= (PciCIS-PciBAR0)/4;
	table = malloc(2*ntb*sizeof(Pcisiz));
	if(table == nil)
		panic("pcibusmap: no memory");
	itb = table;
	mtb = table+ntb;

	/*
	 * Build a table of sizes
	 */
	for(p = root; p != nil; p = p->link) {
		if(p->ccrb == 0x06) {
			if(p->ccru != 0x04 || p->bridge == nil) {
				DBG("pci: ignored bridge %T\n", p->tbdf);
				continue;
			}

			sioa = ioa;
			smema = mema;
			pcibusmap(p->bridge, &smema, &sioa, 0);

			hole = pcimask(smema-mema);
			if(hole < (1<<20))
				hole = 1<<20;
			p->mema.size = hole;

			hole = pcimask(sioa-ioa);
			if(hole < (1<<12))
				hole = 1<<12;

			p->ioa.size = hole;

			itb->dev = p;
			itb->bar = -1;
			itb->siz = p->ioa.size;
			itb++;

			mtb->dev = p;
			mtb->bar = -1;
			mtb->siz = p->mema.size;
			mtb++;
			continue;
		}

		for(i = 0; i <= 5; i++) {
			rno = PciBAR0 + i*4;
			v = pcicfgrw32(p->tbdf, rno, 0, 1);
			size = pcibarsize(p, rno);
			if(size == 0)
				continue;

			if(v & 1) {
				itb->dev = p;
				itb->bar = i;
				itb->siz = size;
				itb++;
			}
			else {
				mtb->dev = p;
				mtb->bar = i;
				mtb->siz = size;
				mtb++;
			}

			p->mem[i].size = size;
		}
	}

	/*
	 * Sort both tables IO smallest first, Memory largest
	 */
	qsort(table, itb-table, sizeof(Pcisiz), pcisizcmp);
	tptr = table+ntb;
	qsort(tptr, mtb-tptr, sizeof(Pcisiz), pcisizcmp);

	/*
	 * Allocate IO address space on this bus
	 */
	for(tptr = table; tptr < itb; tptr++) {
		hole = tptr->siz;
		if(tptr->bar == -1)
			hole = 1<<12;
		ioa = (ioa+hole-1) & ~(hole-1);

		p = tptr->dev;
		if(tptr->bar == -1)
			p->ioa.bar = ioa;
		else {
			p->pcr |= IOen;
			p->mem[tptr->bar].bar = ioa|1;
			if(wrreg)
				pcicfgrw32(p->tbdf, PciBAR0+(tptr->bar*4), ioa|1, 0);
		}

		ioa += tptr->siz;
	}

	/*
	 * Allocate Memory address space on this bus
	 */
	for(tptr = table+ntb; tptr < mtb; tptr++) {
		hole = tptr->siz;
		if(tptr->bar == -1)
			hole = 1<<20;
		mema = (mema+hole-1) & ~(hole-1);

		p = tptr->dev;
		if(tptr->bar == -1)
			p->mema.bar = mema;
		else {
			p->pcr |= MEMen;
			p->mem[tptr->bar].bar = mema;
			if(wrreg)
				pcicfgrw32(p->tbdf, PciBAR0+(tptr->bar*4), mema, 0);
		}
		mema += tptr->siz;
	}

	*pmema = mema;
	*pioa = ioa;
	free(table);

	if(wrreg == 0)
		return;

	/*
	 * Finally set all the bridge addresses & registers
	 */
	for(p = root; p != nil; p = p->link) {
		if(p->bridge == nil) {
			pcicfgrw8(p->tbdf, PciLTR, 64, 0);

			p->pcr |= MASen;
			pcicfgrw16(p->tbdf, PciPCR, p->pcr, 0);
			continue;
		}

		base = p->ioa.bar;
		limit = base+p->ioa.size-1;
		v = pcicfgrw32(p->tbdf, PciIBR, 0, 1);
		v = (v&0xFFFF0000)|(limit & 0xF000)|((base & 0xF000)>>8);
		pcicfgrw32(p->tbdf, PciIBR, v, 0);
		v = (limit & 0xFFFF0000)|(base>>16);
		pcicfgrw32(p->tbdf, PciIUBR, v, 0);

		base = p->mema.bar;
		limit = base+p->mema.size-1;
		v = (limit & 0xFFF00000)|((base & 0xFFF00000)>>16);
		pcicfgrw32(p->tbdf, PciMBR, v, 0);

		/*
		 * Disable memory prefetch
		 */
		pcicfgrw32(p->tbdf, PciPMBR, 0x0000FFFF, 0);
		pcicfgrw8(p->tbdf, PciLTR, 64, 0);

		/*
		 * Enable the bridge
		 */
		p->pcr |= IOen|MEMen|MASen;
		pcicfgrw32(p->tbdf, PciPCR, 0xFFFF0000|p->pcr , 0);

		sioa = p->ioa.bar;
		smema = p->mema.bar;
		pcibusmap(p->bridge, &smema, &sioa, 1);
	}
}

static void
pxh6700(Pcidev *p)
{
	int l;

	/*
	 * errata #3, "signal integrity issues when driving secondary bus
	 * in PCI or PCI-X mode 1 ... (causes) parity errors and system hangs."
	 *
	 * Solution is:
	 *
	 * 1.  Write `1's to Function 0/2 (F0 only for PXH-V) Register
	 * offset 224h Bits [29:17] prior to any PCI bus accesses to
	 * secondary interface.
	 * 2.  Wait at least 1.5 micro seconds (us) before any secondary
	 * bus accesses after setting the register.
	 * 3.  Warm (PCI-E reset) and Cold reset (PWROK reset, Front Panel
	 * reset) will clear these register bits requiring reprogramming.
	 * PXH Secondary Bus Reset will not clear the register.
	 */
	l = pcicfgrw32(p->tbdf, 0x224, 0, 1);
	l |= 0x3FFE0000;
	pcicfgrw32(p->tbdf, 0x224, l, 0);
	microdelay(2);

//	print("6700pxh %uX.%uX: pmode=%d pfreq=%d\n", p->did, p->rid, (l>>14)&3, (l>>9)&3);
}

static void
bridgecfg(Pcidev *p)
{
	int l;

	switch (p->vid<<16 | p->did) {
	case 0x80860329:
	case 0x8086032a:
		pxh6700(p);
//	case 0x80863500:
//	case 0x80863501:
//	case 0x80863502:
//	case 0x80863503:
		/* enable IOxAPIC space */
		l = pcicfgrw16(p->tbdf, 0x40, 0, 1);
		l &= ~(1<<13);
		pcicfgrw16(p->tbdf, 0x40, l, 0);
	}
}

static int
pcilscan(int bno, Pcidev** list)
{
	Pcidev *p, *head, *tail;
	int dno, fno, hdt, l, maxfno, maxubn, sbn, tbdf, ubn;

	maxubn = bno;
	head = nil;
	tail = nil;
	for(dno = 0; dno <= pcimaxdno; dno++){
		maxfno = 0;
		for(fno = 0; fno <= maxfno; fno++){
			/*
			 * For this possible device, form the
			 * bus+device+function triplet needed to address it
			 * and try to read the vendor and device ID.
			 * If successful, allocate a device struct and
			 * start to fill it in with some useful information
			 * from the device's configuration space.
			 */
			tbdf = MKBUS(BusPCI, bno, dno, fno);
			l = pcicfgrw32(tbdf, PciVID, 0, 1);
			if(l == 0xFFFFFFFF || l == 0)
				continue;
			p = malloc(sizeof(*p));
			if(p == nil)
				panic("pcilscan: no memory");
			p->tbdf = tbdf;
			p->vid = l;
			p->did = l>>16;

			if(pcilist != nil)
				pcitail->list = p;
			else
				pcilist = p;
			pcitail = p;

			p->pcr = pcicfgr16(p, PciPCR);
			p->rid = pcicfgr8(p, PciRID);
			p->ccrp = pcicfgr8(p, PciCCRp);
			p->ccru = pcicfgr8(p, PciCCRu);
			p->ccrb = pcicfgr8(p, PciCCRb);
			p->cls = pcicfgr8(p, PciCLS);
			p->ltr = pcicfgr8(p, PciLTR);

			p->intl = pcicfgr8(p, PciINTL);

			/*
			 * If the device is a multi-function device adjust the
			 * loop count so all possible functions are checked.
			 */
			hdt = pcicfgr8(p, PciHDT);
			if(hdt & 0x80)
				maxfno = MaxFNO;

			/*
			 * If appropriate, read the base address registers
			 * and work out the sizes.
			 */
			switch(p->ccrb) {
			default:
				if((hdt & 0x7F) == 0)
					pcibars(p);
				break;

			case 0x06:		/* bridge device */
				bridgecfg(p);
				break;
			case 0x00:
			case 0x05:		/* memory controller */
				break;
			}

			if(head != nil)
				tail->link = p;
			else
				head = p;
			tail = p;
		}
	}

	*list = head;
	for(p = head; p != nil; p = p->link){
		/*
		 * Find PCI-PCI bridges and recursively descend the tree.
		 */
		if(p->ccrb != 0x06 || p->ccru != 0x04)
			continue;

		/*
		 * If the secondary or subordinate bus number is not
		 * initialised try to do what the PCI BIOS should have
		 * done and fill in the numbers as the tree is descended.
		 * On the way down the subordinate bus number is set to
		 * the maximum as it's not known how many buses are behind
		 * this one; the final value is set on the way back up.
		 */
		sbn = pcicfgr8(p, PciSBN);
		ubn = pcicfgr8(p, PciUBN);

		if(sbn == 0 || ubn == 0 || nobios) {
			sbn = maxubn+1;
			/*
			 * Make sure memory, I/O and master enables are
			 * off, set the primary, secondary and subordinate
			 * bus numbers and clear the secondary status before
			 * attempting to scan the secondary bus.
			 *
			 * Initialisation of the bridge should be done here.
			 */
			pcicfgw32(p, PciPCR, 0xFFFF0000);
			l = (MaxUBN<<16)|(sbn<<8)|bno;
			pcicfgw32(p, PciPBN, l);
			pcicfgw16(p, PciSPSR, 0xFFFF);
			maxubn = pcilscan(sbn, &p->bridge);
			l = (maxubn<<16)|(sbn<<8)|bno;

			pcicfgw32(p, PciPBN, l);
		}
		else {
			if(ubn > maxubn)
				maxubn = ubn;
			pcilscan(sbn, &p->bridge);
		}
	}

	return maxubn;
}

int
pciscan(int bno, Pcidev **list)
{
	int ubn;

	lock(&pcicfginitlock);
	ubn = pcilscan(bno, list);
	unlock(&pcicfginitlock);
	return ubn;
}

static uchar
pIIxget(Pcidev *router, uchar link)
{
	uchar pirq;

	/* link should be 0x60, 0x61, 0x62, 0x63 */
	pirq = pcicfgr8(router, link);
	return (pirq < 16)? pirq: 0;
}

static void
pIIxset(Pcidev *router, uchar link, uchar irq)
{
	pcicfgw8(router, link, irq);
}

static uchar
viaget(Pcidev *router, uchar link)
{
	uchar pirq;

	/* link should be 1, 2, 3, 5 */
	pirq = (link < 6)? pcicfgr8(router, 0x55 + (link>>1)): 0;

	return (link & 1)? (pirq >> 4): (pirq & 15);
}

static void
viaset(Pcidev *router, uchar link, uchar irq)
{
	uchar pirq;

	pirq = pcicfgr8(router, 0x55 + (link >> 1));
	pirq &= (link & 1)? 0x0f: 0xf0;
	pirq |= (link & 1)? (irq << 4): (irq & 15);
	pcicfgw8(router, 0x55 + (link>>1), pirq);
}

static uchar
optiget(Pcidev *router, uchar link)
{
	uchar pirq = 0;

	/* link should be 0x02, 0x12, 0x22, 0x32 */
	if ((link & 0xcf) == 0x02)
		pirq = pcicfgr8(router, 0xb8 + (link >> 5));
	return (link & 0x10)? (pirq >> 4): (pirq & 15);
}

static void
optiset(Pcidev *router, uchar link, uchar irq)
{
	uchar pirq;

	pirq = pcicfgr8(router, 0xb8 + (link >> 5));
    	pirq &= (link & 0x10)? 0x0f : 0xf0;
    	pirq |= (link & 0x10)? (irq << 4): (irq & 15);
	pcicfgw8(router, 0xb8 + (link >> 5), pirq);
}

static uchar
aliget(Pcidev *router, uchar link)
{
	/* No, you're not dreaming */
	static const uchar map[] = { 0, 9, 3, 10, 4, 5, 7, 6, 1, 11, 0, 12, 0, 14, 0, 15 };
	uchar pirq;

	/* link should be 0x01..0x08 */
	pirq = pcicfgr8(router, 0x48 + ((link-1)>>1));
	return (link & 1)? map[pirq&15]: map[pirq>>4];
}

static void
aliset(Pcidev *router, uchar link, uchar irq)
{
	/* Inverse of map in aliget */
	static const uchar map[] = { 0, 8, 0, 2, 4, 5, 7, 6, 0, 1, 3, 9, 11, 0, 13, 15 };
	uchar pirq;

	pirq = pcicfgr8(router, 0x48 + ((link-1)>>1));
	pirq &= (link & 1)? 0x0f: 0xf0;
	pirq |= (link & 1)? (map[irq] << 4): (map[irq] & 15);
	pcicfgw8(router, 0x48 + ((link-1)>>1), pirq);
}

static uchar
cyrixget(Pcidev *router, uchar link)
{
	uchar pirq;

	/* link should be 1, 2, 3, 4 */
	pirq = pcicfgr8(router, 0x5c + ((link-1)>>1));
	return ((link & 1)? pirq >> 4: pirq & 15);
}

static void
cyrixset(Pcidev *router, uchar link, uchar irq)
{
	uchar pirq;

	pirq = pcicfgr8(router, 0x5c + (link>>1));
	pirq &= (link & 1)? 0x0f: 0xf0;
	pirq |= (link & 1)? (irq << 4): (irq & 15);
	pcicfgw8(router, 0x5c + (link>>1), pirq);
}

typedef struct Bridge Bridge;
struct Bridge
{
	ushort	vid;
	ushort	did;
	uchar	(*get)(Pcidev *, uchar);
	void	(*set)(Pcidev *, uchar, uchar);
};

static Bridge southbridges[] = {
	{ 0x8086, 0x71a0, nil, nil },		/* intel 440gx */
	{ 0x8086, 0x71a2, nil, nil },		/* intel 440gx */
	{ 0x8086, 0xffff, pIIxget, pIIxset },	/* intel — any */
	{ 0x1106, 0x0586, viaget, viaset },	/* Viatech 82C586 */
	{ 0x1106, 0x0596, viaget, viaset },	/* Viatech 82C596 */
	{ 0x1106, 0x0686, viaget, viaset },	/* Viatech 82C686 */
	{ 0x1106, 0x3227, viaget, viaset },	/* Viatech VT8237 */
	{ 0x1045, 0xc700, optiget, optiset },	/* Opti 82C700 */
	{ 0x10b9, 0x1533, aliget, aliset },	/* Al M1533 */
	{ 0x1039, 0x0008, pIIxget, pIIxset },	/* SI 503 */
	{ 0x1039, 0x0496, pIIxget, pIIxset },	/* SI 496 */
	{ 0x1078, 0x0100, cyrixget, cyrixset },	/* Cyrix 5530 Legacy */
	{ 0x1022, 0x746B, nil, nil },		/* AMD 8111 */
	{ 0x10DE, 0xffff, nil, nil },		/* nVidia — any */
	{ 0x1166, 0x0200, nil, nil },		/* ServerWorks ServerSet III LE */
	{ 0x1002, 0xffff, nil, nil },		/* ATI/AMD — any */
};

typedef struct Slot Slot;
struct Slot {
	uchar	bus;		/* Pci bus number */
	uchar	dev;		/* Pci device number */
	uchar	maps[12];	/* Avoid structs!  Link and mask. */
	uchar	slot;		/* Add-in/built-in slot */
	uchar	reserved;
};

typedef struct Router Router;
struct Router {
	uchar	signature[4];	/* Routing table signature */
	uchar	version[2];	/* Version number */
	uchar	size[2];	/* Total table size */
	uchar	bus;		/* Interrupt router bus number */
	uchar	devfn;		/* Router's devfunc */
	uchar	pciirqs[2];	/* Exclusive PCI irqs */
	uchar	compat[4];	/* Compatible PCI interrupt router */
	uchar	miniport[4];	/* Miniport data */
	uchar	reserved[11];
	uchar	checksum;
};

static ushort pciirqs;		/* Exclusive PCI irqs */
static Bridge *southbridge;	/* Which southbridge to use. */

static void
pcirouting(void)
{
	uchar *p, pin, irq, link, *map;
	int size, i, fn, tbdf;
	Pcidev *sbpci, *pci;
	Router *r;
	Slot *e;

	/* Search for PCI interrupt routing table in BIOS */
	for(p = (uchar*)KADDR(0xf0000); p < (uchar*)KADDR(0xfffff); p += 16)
		if(p[0] == '$' && p[1] == 'P' && p[2] == 'I' && p[3] == 'R')
			break;

	if(p >= (uchar *)KADDR(0xfffff))
		return;

	r = (Router *)p;

	if(0)
		print("PCI interrupt routing table version %d.%d at %#.6ux\n",
			r->version[0], r->version[1], (uint)r & 0xfffff);

	tbdf = (BusPCI << 24)|(r->bus << 16)|(r->devfn << 8);
	sbpci = pcimatchtbdf(tbdf);
	if(sbpci == nil) {
		print("pcirouting: Cannot find south bridge %T\n", tbdf);
		return;
	}

	for(i = 0; i != nelem(southbridges); i++)
		if(sbpci->vid == southbridges[i].vid
		&& (sbpci->did == southbridges[i].did || southbridges[i].did == 0xffff))
			break;

	if(i == nelem(southbridges)) {
		print("pcirouting: ignoring south bridge %T %.4ux/%.4ux\n", tbdf, sbpci->vid, sbpci->did);
		return;
	}
	southbridge = &southbridges[i];
	if(southbridge->get == nil || southbridge->set == nil)
		return;

	pciirqs = r->pciirqs[1]<<8 | r->pciirqs[0];
	size = r->size[1]<<8 | r->size[0];
	for(e = (Slot *)&r[1]; (uchar *)e < p + size; e++) {
		if (0) {
			print("%.2ux/%.2ux %.2ux: ", e->bus, e->dev, e->slot);
			for (i = 0; i != 4; i++) {
				uchar *m = &e->maps[i * 3];
				print("[%d] %.2ux %.4ux ",
					i, m[0], (m[2] << 8)|m[1]);
			}
			print("\n");
		}
		for(fn = 0; fn != 8; fn++) {
			tbdf = MKBUS(BusPCI, e->bus, e->dev, fn);
			pci = pcimatchtbdf(tbdf);
			if(pci == nil)
				continue;
			pin = pcicfgr8(pci, PciINTP);
			if(pin == 0 || pin == 0xff)
				continue;

			map = &e->maps[(pin - 1) * 3];
			link = map[0];
			irq = southbridge->get(sbpci, link);
			if(irq == 0 || irq == pci->intl)
				continue;
			if(pci->intl != 0 && pci->intl != 0xFF) {
				print("pcirouting: %T at pin %d link %d irq %d -> %d\n",
					  tbdf, pin, link, irq, pci->intl);
				southbridge->set(sbpci, link, pci->intl);
				continue;
			}
			print("pcirouting: %T at pin %d link %d irq %d\n", tbdf, pin, link, irq);
			pcicfgw8(pci, PciINTL, irq);
			pci->intl = irq;
		}
	}
}

static void pcireservemem(void);

static int
pcicfgrw8bios(int tbdf, int rno, int data, int read)
{
	BIOS32ci ci;

	if(pcibiossi == nil)
		return -1;

	memset(&ci, 0, sizeof(BIOS32ci));
	ci.ebx = (BUSBNO(tbdf)<<8)|(BUSDNO(tbdf)<<3)|BUSFNO(tbdf);
	ci.edi = rno;
	if(read){
		ci.eax = 0xB108;
		if(!bios32ci(pcibiossi, &ci)/* && !(ci.eax & 0xFF)*/)
			return ci.ecx & 0xFF;
	}
	else{
		ci.eax = 0xB10B;
		ci.ecx = data & 0xFF;
		if(!bios32ci(pcibiossi, &ci)/* && !(ci.eax & 0xFF)*/)
			return 0;
	}

	return -1;
}

static int
pcicfgrw16bios(int tbdf, int rno, int data, int read)
{
	BIOS32ci ci;

	if(pcibiossi == nil)
		return -1;

	memset(&ci, 0, sizeof(BIOS32ci));
	ci.ebx = (BUSBNO(tbdf)<<8)|(BUSDNO(tbdf)<<3)|BUSFNO(tbdf);
	ci.edi = rno;
	if(read){
		ci.eax = 0xB109;
		if(!bios32ci(pcibiossi, &ci)/* && !(ci.eax & 0xFF)*/)
			return ci.ecx & 0xFFFF;
	}
	else{
		ci.eax = 0xB10C;
		ci.ecx = data & 0xFFFF;
		if(!bios32ci(pcibiossi, &ci)/* && !(ci.eax & 0xFF)*/)
			return 0;
	}

	return -1;
}

static int
pcicfgrw32bios(int tbdf, int rno, int data, int read)
{
	BIOS32ci ci;

	if(pcibiossi == nil)
		return -1;

	memset(&ci, 0, sizeof(BIOS32ci));
	ci.ebx = (BUSBNO(tbdf)<<8)|(BUSDNO(tbdf)<<3)|BUSFNO(tbdf);
	ci.edi = rno;
	if(read){
		ci.eax = 0xB10A;
		if(!bios32ci(pcibiossi, &ci)/* && !(ci.eax & 0xFF)*/)
			return ci.ecx;
	}
	else{
		ci.eax = 0xB10D;
		ci.ecx = data;
		if(!bios32ci(pcibiossi, &ci)/* && !(ci.eax & 0xFF)*/)
			return 0;
	}

	return -1;
}

static BIOS32si*
pcibiosinit(void)
{
	BIOS32ci ci;
	BIOS32si *si;

	if((si = bios32open("$PCI")) == nil)
		return nil;

	memset(&ci, 0, sizeof(BIOS32ci));
	ci.eax = 0xB101;
	if(bios32ci(si, &ci) || ci.edx != ((' '<<24)|('I'<<16)|('C'<<8)|'P')){
		free(si);
		return nil;
	}
	if(ci.eax & 0x01)
		pcimaxdno = 31;
	else
		pcimaxdno = 15;
	pcimaxbno = ci.ecx & 0xff;

	return si;
}

void
pcibussize(Pcidev *root, ulong *msize, ulong *iosize)
{
	*msize = 0;
	*iosize = 0;
	pcibusmap(root, msize, iosize, 0);
}

static void
pcireservemem(void)
{
	int i;
	Pcidev *p;
	
	for(p = nil; p = pcimatch(p, 0, 0); )
		for(i=0; i<nelem(p->mem); i++)
			if(p->mem[i].bar && (p->mem[i].bar&1) == 0)
				upareserve(p->mem[i].bar&~(uintmem)0xf, p->mem[i].size);
}

static void
pcicfginit(void)
{
	char *p;
	Pcidev **list;
	ulong mema, ioa;
	int bno, n, pcibios;

	lock(&pcicfginitlock);
	if(pcicfgmode != -1)
		goto out;

	pcibios = 0;
	if(getconf("*nobios"))
		nobios = 1;
	else if(getconf("*pcibios"))
		pcibios = 1;
	if(getconf("*nopcirouting"))
		nopcirouting = 1;

	/*
	 * Try to determine which PCI configuration mode is implemented.
	 * Mode2 uses a byte at 0xCF8 and another at 0xCFA; Mode1 uses
	 * a DWORD at 0xCF8 and another at 0xCFC and will pass through
	 * any non-DWORD accesses as normal I/O cycles. There shouldn't be
	 * a device behind these addresses so if Mode1 accesses fail try
	 * for Mode2 (Mode2 is deprecated).
	 */
	if(!pcibios){
		/*
		 * Bits [30:24] of PciADDR must be 0,
		 * according to the spec.
		 */
		n = inl(PciADDR);
		if(!(n & 0x7F000000)){
			outl(PciADDR, 0x80000000);
			outb(PciADDR+3, 0);
			if(inl(PciADDR) & 0x80000000){
				pcicfgmode = 1;
				pcimaxdno = 31;
			}
		}
		outl(PciADDR, n);

		if(pcicfgmode < 0){
			/*
			 * The 'key' part of PciCSE should be 0.
			 */
			n = inb(PciCSE);
			if(!(n & 0xF0)){
				outb(PciCSE, 0x0E);
				if(inb(PciCSE) == 0x0E){
					pcicfgmode = 2;
					pcimaxdno = 15;
				}
			}
			outb(PciCSE, n);
		}
	}

	if(pcicfgmode < 0 || pcibios) {
		if((pcibiossi = pcibiosinit()) == nil)
			goto out;
		pcicfgrw8 = pcicfgrw8bios;
		pcicfgrw16 = pcicfgrw16bios;
		pcicfgrw32 = pcicfgrw32bios;
		pcicfgmode = 3;
	}

	fmtinstall('T', fmtT);

	if(p = getconf("*pcimaxbno"))
		pcimaxbno = strtoul(p, 0, 0);
	if(p = getconf("*pcimaxdno")){
		n = strtoul(p, 0, 0);
		if(n < pcimaxdno)
			pcimaxdno = n;
	}

	list = &pciroot;
	for(bno = 0; bno <= pcimaxbno; bno++) {
		int sbno = bno;
		bno = pcilscan(bno, list);

		while(*list)
			list = &(*list)->link;

		if (sbno == 0) {
			Pcidev *pci;

			/*
			  * If we have found a PCI-to-Cardbus bridge, make sure
			  * it has no valid mappings anymore.
			  */
			for(pci = pciroot; pci != nil; pci = pci->link){
				if (pci->ccrb == 6 && pci->ccru == 7) {
					ushort bcr;

					/* reset the cardbus */
					bcr = pcicfgr16(pci, PciBCR);
					pcicfgw16(pci, PciBCR, 0x40 | bcr);
					delay(50);
				}
			}
		}
	}

	if(pciroot == nil)
		goto out;

	if(nobios) {
		/*
		 * Work out how big the top bus is
		 */
		pcibussize(pciroot, &mema, &ioa);

		/*
		 * Align the windows and map it
		 */
		ioa = 0x1000;
		mema = 0x90000000;

		DBG("Mask sizes: mem=%lux io=%lux\n", mema, ioa);

		pcibusmap(pciroot, &mema, &ioa, 1);
		DBG("Sizes2: mem=%lux io=%lux\n", mema, ioa);

		unlock(&pcicfginitlock);
		return;
	}

	if (!nopcirouting)
		pcirouting();

out:
	pcireservemem();
	unlock(&pcicfginitlock);

	if(getconf("*pcihinv"))
		pcihinv(nil);
}

static int
pcicfgrw8raw(int tbdf, int rno, int data, int read)
{
	int o, type, x;

	if(pcicfgmode == -1)
		pcicfginit();

	if(BUSBNO(tbdf))
		type = 0x01;
	else
		type = 0x00;
	x = -1;
	if(BUSDNO(tbdf) > pcimaxdno)
		return x;

	lock(&pcicfglock);
	switch(pcicfgmode){

	case 1:
		o = rno & 0x03;
		rno &= ~0x03;
		outl(PciADDR, 0x80000000|BUSBDF(tbdf)|rno|type);
		if(read)
			x = inb(PciDATA+o);
		else
			outb(PciDATA+o, data);
		outl(PciADDR, 0);
		break;

	case 2:
		outb(PciCSE, 0x80|(BUSFNO(tbdf)<<1));
		outb(PciFORWARD, BUSBNO(tbdf));
		if(read)
			x = inb((0xC000|(BUSDNO(tbdf)<<8)) + rno);
		else
			outb((0xC000|(BUSDNO(tbdf)<<8)) + rno, data);
		outb(PciCSE, 0);
		break;
	}
	unlock(&pcicfglock);

	return x;
}

int
pcicfgr8(Pcidev* pcidev, int rno)
{
	return pcicfgrw8(pcidev->tbdf, rno, 0, 1);
}

void
pcicfgw8(Pcidev* pcidev, int rno, int data)
{
	pcicfgrw8(pcidev->tbdf, rno, data, 0);
}

static int
pcicfgrw16raw(int tbdf, int rno, int data, int read)
{
	int o, type, x;

	if(pcicfgmode == -1)
		pcicfginit();

	if(BUSBNO(tbdf))
		type = 0x01;
	else
		type = 0x00;
	x = -1;
	if(BUSDNO(tbdf) > pcimaxdno)
		return x;

	lock(&pcicfglock);
	switch(pcicfgmode){

	case 1:
		o = rno & 0x02;
		rno &= ~0x03;
		outl(PciADDR, 0x80000000|BUSBDF(tbdf)|rno|type);
		if(read)
			x = ins(PciDATA+o);
		else
			outs(PciDATA+o, data);
		outl(PciADDR, 0);
		break;

	case 2:
		outb(PciCSE, 0x80|(BUSFNO(tbdf)<<1));
		outb(PciFORWARD, BUSBNO(tbdf));
		if(read)
			x = ins((0xC000|(BUSDNO(tbdf)<<8)) + rno);
		else
			outs((0xC000|(BUSDNO(tbdf)<<8)) + rno, data);
		outb(PciCSE, 0);
		break;
	}
	unlock(&pcicfglock);

	return x;
}

int
pcicfgr16(Pcidev* pcidev, int rno)
{
	return pcicfgrw16(pcidev->tbdf, rno, 0, 1);
}

void
pcicfgw16(Pcidev* pcidev, int rno, int data)
{
	pcicfgrw16(pcidev->tbdf, rno, data, 0);
}

static int
pcicfgrw32raw(int tbdf, int rno, int data, int read)
{
	int type, x;

	if(pcicfgmode == -1)
		pcicfginit();

	if(BUSBNO(tbdf))
		type = 0x01;
	else
		type = 0x00;
	x = -1;
	if(BUSDNO(tbdf) > pcimaxdno)
		return x;

	lock(&pcicfglock);
	switch(pcicfgmode){

	case 1:
		rno &= ~0x03;
		outl(PciADDR, 0x80000000|BUSBDF(tbdf)|rno|type);
		if(read)
			x = inl(PciDATA);
		else
			outl(PciDATA, data);
		outl(PciADDR, 0);
		break;

	case 2:
		outb(PciCSE, 0x80|(BUSFNO(tbdf)<<1));
		outb(PciFORWARD, BUSBNO(tbdf));
		if(read)
			x = inl((0xC000|(BUSDNO(tbdf)<<8)) + rno);
		else
			outl((0xC000|(BUSDNO(tbdf)<<8)) + rno, data);
		outb(PciCSE, 0);
		break;
	}
	unlock(&pcicfglock);

	return x;
}

int
pcicfgr32(Pcidev* pcidev, int rno)
{
	return pcicfgrw32(pcidev->tbdf, rno, 0, 1);
}

void
pcicfgw32(Pcidev* pcidev, int rno, int data)
{
	pcicfgrw32(pcidev->tbdf, rno, data, 0);
}

Pcidev*
pcimatch(Pcidev* prev, int vid, int did)
{
	if(pcicfgmode == -1)
		pcicfginit();
	prev = prev? prev->list: pcilist;
	for(; prev != nil; prev = prev->list){
		if((vid == 0 || prev->vid == vid)
		&& (did == 0 || prev->did == did))
			break;
	}
	return prev;
}

Pcidev*
pcimatchtbdf(int tbdf)
{
	Pcidev *p;

	if(pcicfgmode == -1)
		pcicfginit();

	for(p = pcilist; p != nil; p = p->list) {
		if(p->tbdf == tbdf)
			break;
	}
	return p;
}

uchar
pciipin(Pcidev *pci, uchar pin)
{
	if (pci == nil)
		pci = pcilist;

	while (pci) {
		uchar intl;

		if (pcicfgr8(pci, PciINTP) == pin && pci->intl != 0 && pci->intl != 0xff)
			return pci->intl;

		if (pci->bridge && (intl = pciipin(pci->bridge, pin)) != 0)
			return intl;

		pci = pci->list;
	}
	return 0;
}

void
pcihinv(Pcidev* p)
{
	int i;
	Pcidev *t;

	if(p == nil) {
		p = pciroot;
		print("tbdf:	 type vid  did intl memory\n");
	}
	for(t = p; t != nil; t = t->link) {
		print("%T:	%.2ux %.4ux/%.4ux %.2d",
			t->tbdf, t->ccru, t->vid, t->did, t->intl);

		for(i = 0; i < nelem(p->mem); i++) {
			if(t->mem[i].size == 0)
				continue;
			print("%d:%#P %d ", i, t->mem[i].bar, t->mem[i].size);
		}
		if(t->bridge != nil)
			print("->%d", BUSBNO(t->bridge->tbdf));
		print("\n");
	}
	for(; p != nil; p = p->link)
		if(p->bridge != nil)
			pcihinv(p->bridge);
}

void
pcireset(void)
{
	Pcidev *p;

	if(pcicfgmode == -1)
		pcicfginit();

	for(p = pcilist; p != nil; p = p->list) {
		/* don't mess with the bridges */
		if(p->ccrb == 0x06)
			continue;
		pciclrbme(p);
	}
}

void
pcisetioe(Pcidev* p)
{
	p->pcr |= IOen;
	pcicfgw16(p, PciPCR, p->pcr);
}

void
pciclrioe(Pcidev* p)
{
	p->pcr &= ~IOen;
	pcicfgw16(p, PciPCR, p->pcr);
}

void
pcisetbme(Pcidev* p)
{
	p->pcr |= MASen;
	pcicfgw16(p, PciPCR, p->pcr);
}

void
pciclrbme(Pcidev* p)
{
	p->pcr &= ~MASen;
	pcicfgw16(p, PciPCR, p->pcr);
}

void
pcisetmwi(Pcidev* p)
{
	p->pcr |= MemWrInv;
	pcicfgw16(p, PciPCR, p->pcr);
}

void
pciclrmwi(Pcidev* p)
{
	p->pcr &= ~MemWrInv;
	pcicfgw16(p, PciPCR, p->pcr);
}

int
pcicap(Pcidev *p, int cap)
{
	int i, c, off;

	/* status register bit 4 has capabilities */
	if((pcicfgr16(p, PciPSR) & 1<<4) == 0)
		return -1;	
	switch(pcicfgr8(p, PciHDT) & 0x7f){
	default:
		return -1;
	case 0:				/* etc */
	case 1:				/* pci to pci bridge */
		off = 0x34;
		break;
	case 2:				/* cardbus bridge */
		off = 0x14;
		break;
	}
	for(i = 48; i--;){
		off = pcicfgr8(p, off);
		if(off < 0x40 || (off & 3))
			break;
		off &= ~3;
		c = pcicfgr8(p, off);
		if(c == 0xff)
			break;
		if(c == cap)
			return off;
		off++;
	}
	return -1;
}

enum {
	Pmgcap	= 2,		/* capabilities; 2 bytes*/
	Pmgctl	= 4,		/* ctl/status; 2 bytes */
	Pmgbrg	= 6,		/* bridge support */
	Pmgdata	= 7,
};

int
pcigetpms(Pcidev* p)
{
	int ptr;

	if((ptr = pcicap(p, PciCapPMG)) == -1)
		return -1;
	return pcicfgr16(p, ptr+Pmgctl) & 0x0003;
}

int
pcisetpms(Pcidev* p, int state)
{
	int pmc, pmcsr, ptr;

	if((ptr = pcicap(p, PciCapPMG)) == -1)
		return -1;

	pmc = pcicfgr16(p, ptr+Pmgcap);
	pmcsr = pcicfgr16(p, ptr+Pmgctl);

	switch(state){
	default:
		return -1;
	case 0:
		break;
	case 1:
		if(!(pmc & 0x0200))
			return -1;
		break;
	case 2:
		if(!(pmc & 0x0400))
			return -1;
		break;
	case 3:
		break;
	}
	pcicfgw16(p, ptr+4, (pmcsr & ~3)  | state);
	return pmcsr & 3;
}
