#include <assert.h>
#include <err.h>
#include <getopt.h>
#include <sysexits.h>
#include "fmd.h"

static void
usage(void)
{
	puts("Usage: fmdscan [-r] <path>");
}


int
main(int argc, char *argv[])
{
	int r_flag = 0, opt;
	while ((opt = getopt(argc, argv, "rh")) != -1)
		switch (opt) {
		case 'r': r_flag = 1; break;
		case 'h': usage(); return 0;
		case '*': return EX_USAGE;
		}
	argc -= optind;
	argv += optind;
	if (argc == 0)
		return usage(), EX_USAGE;

	enum FmdScanFlags flags = fmdsf_single | fmdsf_metadata;
	if (r_flag)
		flags |= fmdsf_recursive;
	int i;
	for (i = 0; i < argc; ++i) {
		struct FmdFile *file = 0;
		int res = fmd_scan(argv[i], flags, &file);
		if (res == -1)
			err(EX_OSERR, "%s", argv[i]);
		struct FmdFile *it;
		for (it = file; it; it = it->next)
			fmd_print_file(it, 1, stdout);
		fmd_free_chain(file);
	}
	return 0;
}
