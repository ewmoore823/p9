enum {
	MaxEther	= 48,
	Ntypes		= 8,
};

typedef struct Ether Ether;
struct Ether {
	ISAConf;			/* hardware info */

	int	ctlrno;
	uchar	ea[Eaddrlen];

	void	(*attach)(Ether*);	/* filled in by reset routine */
	void	(*detach)(Ether*);
	void	(*transmit)(Ether*);
	void	(*interrupt)(Ureg*, void*);
	long	(*ifstat)(Ether*, void*, long, ulong);
	long 	(*ctl)(Ether*, void*, long); /* custom ctl messages */
	void	(*power)(Ether*, int);	/* power on/off */
	void	(*shutdown)(Ether*);	/* shutdown hardware before reboot */
	void	*ctlr;

	Queue*	oq;

	Netif;
};

Block*	etheriq(Ether*, Block*, int);
void	addethercard(char*, int(*)(Ether*));
uint	ethercrc(uchar*, int);
int	parseether(uchar*, char*);
int	ethercfgmatch(Ether*, Pcidev*, uintmem);

#define NEXT(x, l)	(((x)+1)%(l))
#define PREV(x, l)	(((x) == 0) ? (l)-1: (x)-1)
