/*
 * The following command strings are passed to sh to be evaluated every
 * `period` seconds (unless `period` is 0). Send signal X to `sblocks`
 * to run again the command and update the blocks with signal X.
 * Also notice that other signals might kill it.
 */

#define TOSIG(X) (31 - (X))

static const Blk blks[] = {
	/* before  command            after  period  signal */
	{ "\1",    "STATUS_player",   "",    1,      TOSIG(1) },
	{ "\1 ",   "STATUS_kblayout", "",    2,      TOSIG(2) },
	{ "\1 ",   "STATUS_network",  "",    10,     TOSIG(5) },
	{ "\1 ",   "STATUS_volume",   "",    10,     TOSIG(4) },
	{ "\1 ",   "STATUS_battery",  "",    10,     TOSIG(2) },
	{ "\1 ",   "STATUS_date",     "",    1,      0 },
};
