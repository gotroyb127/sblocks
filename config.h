/*
 * The following command strings are passed to sh to be evaluated every
 * `period` seconds (unless `period` is 0). Send signal X to `sblocks`
 * to run again the command and update the blocks with signal X.
 * Also notice that other signals might kill it.
 * The last field is the timespec (secs, nanosecs) to wait until
 * command finishes. If the time expires and the command has not
 * finished, its text is not updated on that loop, but on the
 * first after it has finished. You are can use time(1) to
 * estimate how much is useful.
 */

/* time for each cycle */
#define SEC      1
#define NSEC     0
#define TOSIG(X) (31 - (X))

static const Blk blks[] = {
	/* before  command            after  period  signal    secs, nanosecs */
	{ "\1",    "STATUS_player",   "",    1,      TOSIG(1), { 0, 45E7 } },
	{ "\1 ",   "STATUS_kblayout", "",    2,      TOSIG(2), { 0, 4E7 } },
	{ "\1 ",   "STATUS_network",  "",    10,     TOSIG(3), { 0, 1E8 } },
	{ "\1 ",   "STATUS_volume",   "",    10,     TOSIG(4), { 0, 2E8 } },
	{ "\1 ",   "STATUS_battery",  "",    10,     TOSIG(5), { 0, 5E7 } },
	{ "\1 ",   "STATUS_date",     "",    1,      0,        { 0, 4E7 } },
};
