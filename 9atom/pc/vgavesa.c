/*
 * vga driver using just vesa bios to set up.
 *
 * note that setting hwaccel to zero will cause cursor ghosts to be
 * left behind.  hwaccel set non-zero repairs this.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"
#include "ureg.h"

#define	Image	IMAGE
#include <draw.h>
#include <memdraw.h>
#include <cursor.h>
#include "screen.h"

static void *hardscreen;
static uchar modebuf[0x1000];

#define WORD(p) ((p)[0] | ((p)[1]<<8))
#define LONG(p) ((p)[0] | ((p)[1]<<8) | ((p)[2]<<16) | ((p)[3]<<24))
#define PWORD(p, v) (p)[0] = (v); (p)[1] = (v)>>8
#define PLONG(p, v) (p)[0] = (v); (p)[1] = (v)>>8; (p)[2] = (v)>>16; (p)[3] = (v)>>24

extern void realmode(Ureg*);

static uchar*
vbesetup(Ureg *u, int ax)
{
	ulong pa;

	pa = PADDR(RMBUF);
	memset(modebuf, 0, sizeof modebuf);
	memset(u, 0, sizeof *u);
	u->ax = ax;
	u->es = (pa>>4)&0xF000;
	u->di = pa&0xFFFF;
	return modebuf;
}

/* botch.  use bios directly.  called from mouseclock without a up */
static int
vbecall0(Ureg *u)
{
	u->trap = 0x10;
	realmode(u);
	if((u->ax&0xFFFF) != 0x004F)
		return -1;
	return 0;
}

static void
vbecall(Ureg *u)
{
	Chan *creg, *cmem;
	ulong pa;

	cmem = namec("/dev/realmodemem", Aopen, ORDWR, 0);
	if(waserror()){
		cclose(cmem);
		nexterror();
	}
	creg = namec("/dev/realmode", Aopen, ORDWR, 0);
	if(waserror()){
		cclose(creg);
		nexterror();
	}
	pa = PADDR(RMBUF);
	/* TODO: check read and write return values */
	devtab[cmem->type]->write(cmem, modebuf, sizeof modebuf, pa);
	u->trap = 0x10;
	devtab[creg->type]->write(creg, u, sizeof *u, 0);

	devtab[creg->type]->read(creg, u, sizeof *u, 0);
	if((u->ax&0xFFFF) != 0x004F)
		error("vesa bios error");
	devtab[cmem->type]->read(cmem, modebuf, sizeof modebuf, pa);

	poperror();
	cclose(creg);
	poperror();
	cclose(cmem);
}

static void
vbecheck(void)
{
	Ureg u;
	uchar *p;

	p = vbesetup(&u, 0x4F00);
	strcpy((char*)p, "VBE2");
	vbecall(&u);
	if(memcmp((char*)p, "VESA", 4) != 0)
		error("bad vesa signature");
	if(p[5] < 2)
		error("bad vesa version");
}

static int
vbegetmode(void)
{
	Ureg u;

	vbesetup(&u, 0x4F03);
	vbecall(&u);
	return u.bx;
}

static uchar*
vbemodeinfo(int mode)
{
	uchar *p;
	Ureg u;

	p = vbesetup(&u, 0x4F01);
	u.cx = mode;
	vbecall(&u);
	return p;
}

static void
vesalinear(VGAscr *scr, int, int)
{
	int i, mode, size;
	char *s;
	uchar *p;
	uintmem pa;
	Pcidev *pci;

	if(hardscreen) {
		scr->vaddr = 0;
		scr->paddr = scr->apsize = 0;
		return;
	}

	vbecheck();
	mode = vbegetmode();
	/*
	 * bochs loses the top bits - cannot use this
	if((mode&(1<<14)) == 0)
		error("not in linear graphics mode");
	 */
	mode &= 0x3FFF;
	p = vbemodeinfo(mode);
	if(!(WORD(p+0) & (1<<4)))
		error("not in VESA graphics mode");
	if(!(WORD(p+0) & (1<<7)))
		error("not in linear graphics mode");

	pa = LONG(p+40);
	size = WORD(p+20)*WORD(p+16);
	size = PGROUND(size);

	/*
	 * figure out max size of memory so that we have
	 * enough if the screen is resized.
	 */
	pci = nil;
	while((pci = pcimatch(pci, 0, 0)) != nil){
		if(pci->ccrb != Pcibcdisp)
			continue;
		for(i=0; i<nelem(pci->mem); i++)
			if(pa == (pci->mem[i].bar&~(uintmem)0xf)){
				if(pci->mem[i].size > size)
					size = pci->mem[i].size;
				goto havesize;
			}
	}

	/* no pci - heuristic guess */
	if(size < 4*1024*1024)
		size = 4*1024*1024;
	else
		size = ROUND(size, 1024*1024);

havesize:
	vgalinearaddr(scr, pa, size);
	if(scr->apsize)
		addvgaseg("vesascreen", scr->paddr, scr->apsize);

	s = getconf("*vesahardscreen");
	if(s == nil || atoi(s) == 0){
		hardscreen = scr->vaddr;
		scr->vaddr = 0;
		scr->paddr = scr->apsize = 0;
	}
}

static void
vesaflush(VGAscr *scr, Rectangle r)
{
	int t, w, wid, off;
	ulong *hp, *sp, *esp;

	if(hardscreen == nil)
		return;
	if(rectclip(&r, scr->gscreen->r) == 0)
		return;
	sp = (ulong*)(scr->gscreendata->bdata + scr->gscreen->zero);
	t = (r.max.x * scr->gscreen->depth + 2*BI2WD-1) / BI2WD;
	w = (r.min.x * scr->gscreen->depth) / BI2WD;
	w = (t - w) * BY2WD;
	wid = scr->gscreen->width;
	off = r.min.y * wid + (r.min.x * scr->gscreen->depth) / BI2WD;

	hp = hardscreen;
	hp += off;
	sp += off;
	esp = sp + Dy(r) * wid;
	while(sp < esp){
		memmove(hp, sp, w);
		hp += wid;
		sp += wid;
	}
}

static void
vesablank(VGAscr*, int blank)
{
	Ureg u;

	vbesetup(&u, 0x4f10);
	if(blank)
		u.bx = 0x0101;
	else
		u.bx = 0x0001;
	vbecall0(&u);		/* ignore errors */
}

static void
vesadrawinit(VGAscr *scr)
{
	scr->blank = vesablank;
	hwblank = 1;
}

VGAdev vgavesadev = {
	"vesa",
	0,
	0,
	0,
	vesalinear,
	vesadrawinit,
	0,
	0,
	0,
	vesaflush,
};
