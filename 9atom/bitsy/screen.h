typedef struct Cursorinfo Cursorinfo;
typedef struct Cursor Cursor;

extern ulong blanktime;

struct Cursorinfo {
	Lock;
};

extern void	blankscreen(int);
extern void	flushmemscreen(Rectangle);
extern uchar*	attachscreen(Rectangle*, ulong*, int*, int*, int*);
extern	int	setcolor(ulong, ulong, ulong, ulong);
extern	void	getcolor(ulong, ulong*, ulong*, ulong*);

#define ishwimage(i)	0
