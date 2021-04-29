#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <X11/Xlib.h>
#include <pthread.h>

#include "arg.h"

/* macros */
#define BLKLEN     256
#define BLKN       LENGTH(blks)
#define CLOCK      CLOCK_MONOTONIC
#define LENGTH(X)  (sizeof X / sizeof X[0])
#define MAX(X,Y)   (X > Y ? (X) : (Y))
#define FROMSIG(X) (31 - (X))
#define TOSIG(X)   (31 - (X))
#define SEC        1
#define NSEC       0
#define MIN_NSEC   1E6
#define SIGMAX     31
#define STSLEN     512

#ifdef DEBUG
#define debugf(...) eprintf(__VA_ARGS__)
#else
#define debugf(...)
#endif /* DEBUG */
#define TsDebug(t)  debugf("%s: %ld.%09ld\n", #t, t->tv_sec, t->tv_nsec);
#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define esigaction(sig, act, oldact)\
	do {\
		if (sigaction(sig, act, oldact))\
			eprintf("sblocks[%d]: sigaction: '%s' on signal %d\n",\
			        __LINE__, strerror(errno), sig);\
	} while (0)
#define setsighandler(s, h)\
	esigaction(s, &(struct sigaction){.sa_handler = h}, NULL)

typedef struct {
	char *strBefore;
	char *cmd;
	char *strAfter;
	unsigned int period;
	unsigned int sig;
} Blk;

#ifdef THR
typedef struct {
	pthread_t t;
	int tojoin;
	int idx;
} Thrd;
#endif /* THR */

/* function declarations */
static void BlkstrToOutStr(void);
static void CloseDisplay(void);
static void OnQuit(int s);
static void OpenDisplay(void);
static void (*PrintOutStr)(void);
static void Run(void);
static void SetRoot(void);
static void SigHan(int s);
static void SigSetup(void);
static void Sleep();
static void StdoutPrint(void);
static void TsDiff(struct timespec *res, const struct timespec *a, const struct timespec *b);
static int UpdateAll(int t);
static void UpdateBlk(int i);
#ifdef THR
static void AsyncUpdateBlk(int i);
static void InitThrds(void);
static void SyncUpdatedBlks(void);
static void *ThrdUpdateBlk(void *arg);
#endif /* THR */

/* include configuration file before variable declerations */
#include "config.h"

/* variables */
char *argv0;
static char blkstr[BLKN][BLKLEN];
static char OutStr[STSLEN];
static int T = -1;
static int Restart = 0;
static int Running = 1;
static int LastSignal = 0;
static struct timespec *next_ts = &(struct timespec) { 0, 0 };
static struct timespec *curr_ts = &(struct timespec) { 0, 0 };
static struct timespec *sleep_ts = &(struct timespec) { 0, 0 };
/* X11 specific */
static Display *dpy;
static Window root;
static int screen;
#ifdef THR
static Thrd thrds[BLKN];
#endif /* THR */

/* function implementations */
void
BlkstrToOutStr(void)
{
	int i, e;

	for (i = e = 0; i < BLKN; ++i) {
		e += sprintf(OutStr + e, "%s%s%s",
		     blks[i].strBefore, blkstr[i], blks[i].strAfter);
	}
}

void
CloseDisplay(void)
{
	XCloseDisplay(dpy);
}

void
OnQuit(int s)
{
	Running = 0;
	if (s == SIGHUP) /* restart */
		Restart = 1;
}

void
OpenDisplay(void)
{
	if (!(dpy = XOpenDisplay(NULL))) {
		eprintf("sblocks: cannot open display\n");
		exit(1);
	}

	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
}

void
Run(void)
{
	if (PrintOutStr == SetRoot)
		OpenDisplay();

	while (Running) {
		T += 1;
		clock_gettime(CLOCK, curr_ts);
		next_ts->tv_sec = curr_ts->tv_sec + SEC;
		next_ts->tv_nsec = curr_ts->tv_nsec + NSEC;
		if (UpdateAll(T)) {
			BlkstrToOutStr();
			PrintOutStr();
		}
		Sleep();
	}

	if (PrintOutStr == SetRoot)
		CloseDisplay();
}

void
SetRoot(void)
{
	XStoreName(dpy, root, OutStr);
	XFlush(dpy);
}

void
SigHan(int s)
{
	LastSignal = FROMSIG(s);
}

void
SigSetup(void)
{
	int i, s;

	for (s = 16; s <= SIGMAX; ++s) {
		if (s != SIGSTOP)
			setsighandler(s, SIG_IGN);
	}

	setsighandler(SIGINT, OnQuit);
	setsighandler(SIGTERM, OnQuit);
	setsighandler(SIGHUP, OnQuit);

	setsighandler(SIGPIPE, SigHan);

	for (i = 0; i < BLKN; ++i) {
		s = TOSIG(blks[i].sig);
		if (s != 0)
			setsighandler(s, SigHan);
	}
}

void
Sleep(void)
{
	int i;

	if (!Running)
		return;
	if (LastSignal) {
		for (i = 0; i < BLKN; ++i) {
			if (blks[i].sig == LastSignal)
#ifdef THR
				AsyncUpdateBlk(i);
#else
				UpdateBlk(i);
#endif /* THR */
		}
#ifdef THR
		SyncUpdatedBlks();
#endif /* THR */
		BlkstrToOutStr();
		PrintOutStr();
		LastSignal = 0;
	}

	clock_gettime(CLOCK, curr_ts);
	TsDiff(sleep_ts, next_ts, curr_ts);
	TsDebug(sleep_ts);
	if (nanosleep(sleep_ts, NULL))
		Sleep();
}

void
StdoutPrint(void)
{
	puts(OutStr);
}

void
TsDiff(struct timespec *res, const struct timespec *a, const struct timespec *b)
{
	res->tv_sec = a->tv_sec - b->tv_sec - (a->tv_nsec < b->tv_nsec);
	res->tv_nsec = a->tv_nsec - b->tv_nsec +
	               (a->tv_nsec < b->tv_nsec) * 1E9;
	/* needed after continiously signaling */
	res->tv_nsec = MAX(res->tv_nsec, MIN_NSEC);
	res->tv_sec = MAX(res->tv_sec, 0);
}

int
UpdateAll(int t)
{
	int i, per, u;

	for (i = u = 0; i < BLKN; ++i) {
		per = blks[i].period;
		if ((per != 0 && t % per == 0) || t == 0) {
#ifdef THR
			AsyncUpdateBlk(i);
#else
			UpdateBlk(i);
#endif /* THR */
			u = 1;
		}
	}
#ifdef THR
	SyncUpdatedBlks();
#endif /* THR */

	return u;
}

#ifdef THR
void
AsyncUpdateBlk(int i)
{
	pthread_create(&thrds[i].t, NULL, ThrdUpdateBlk, &thrds[i].idx);
	thrds[i].tojoin = 1;
}

void
InitThrds(void)
{
	int i;
	for (i = 0; i < BLKN; ++i)
		thrds[i].idx = i;
}

void
SyncUpdatedBlks(void)
{
	int i;
	for (i = 0; i < BLKN; ++i) {
		if (thrds[i].tojoin) {
			pthread_join(thrds[i].t, NULL);
			thrds[i].tojoin = 0;
		}
	}
}

void *
ThrdUpdateBlk(void *arg)
{
	int i = *(int*)arg;

	debugf("in: blk[%d]\n", i);
	UpdateBlk(i);
	debugf("out: blk[%d]\n", i);

	return NULL;
}
#endif /* THR */

void
UpdateBlk(int i)
{
	FILE *cmdout;

	cmdout = popen(blks[i].cmd, "r");
	if (!fgets(blkstr[i], BLKLEN, cmdout) && !LastSignal)
		*blkstr[i] = '\0';
	pclose(cmdout);
}

int
main(int argc, char *argv[])
{
	PrintOutStr = SetRoot;

	ARGBEGIN {
	case 'o':
		PrintOutStr = StdoutPrint;
		break;
	default:
		fprintf(stderr, "usage: %s [-o]\n", argv0);
		exit(1);
	} ARGEND

#ifdef THR
	InitThrds();
#endif /* THR */
	SigSetup();
	Run();

	if (Restart)
		execvp(argv[0], argv);
	OnQuit(SIGTERM);

	return 0;
}
