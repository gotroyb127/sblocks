/* The following command strings are passed to sh to be evaluated. Signal X is used to update
 * the appropriate block when `sblocks` receives TOSIG(X) (see macro). Defaults to (31 - (X)),
 * no need for RTMIN signals that Linux provide, as it overrides other signals (unsure if it is safe).
 */

static const Blk blks[] = {
	/* before  command             after  period    signal */
	{ "",      "STATUS_player",    "",    1,        1 },
	{ " ",     "STATUS_kblayout",  "",    2,        2 },
	{ " ",     "STATUS_network",   "",    10,       5 },
	{ " ",     "STATUS_volume",    "",    10,       4 },
	{ " ",     "STATUS_battery",   "",    10,       2 },
	{ " ",     "STATUS_date",      "",    1,        0 },
};
