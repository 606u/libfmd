#include <assert.h>
#include <err.h>
#include <getopt.h>
#include <stdarg.h>
#include <string.h>
#include <sysexits.h>
#include "fmd.h"

static void
usage(void)
{
	puts("Usage: fmdscan [-r] <path>");
}


static void
log_hook(struct FmdScanJob *job,
	 const char *path,
	 enum FmdLogType lt,
	 const char *fmt, ...)
{
	static const char *label[] = { "trc", "fmt", "ose", "use" };
	assert(job); (void)job;
	assert(path); (void)path;
	(void)lt;
	assert(fmt);

	fprintf(stderr, "fmdscan[%s]: ", label[lt]);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static int
begin_hook(struct FmdScanJob *job, const char *path)
{
	assert(job); (void)job;
	assert(path);
	fprintf(stderr, "\rScanning '%s'...   ", path);
	fflush(stderr);
	return 0;
}

static int
finish_hook(struct FmdScanJob *job, struct FmdFile *file)
{
	assert(job); (void)job;
	assert(file);
	fprintf(stderr, "\rFinished '%s'.   ", file->path);
	fflush(stderr);
	return 0;
}


int
main(int argc, char *argv[])
{
	int r_flag = 0, m_flag = 0, opt;
	while ((opt = getopt(argc, argv, "rmh")) != -1)
		switch (opt) {
		case 'r': r_flag = 1; break;
		case 'm': m_flag = 1; break;
		case 'h': usage(); return 0;
		case '?': return EX_USAGE;
		}
	argc -= optind;
	argv += optind;
	if (argc == 0)
		return usage(), EX_USAGE;

	struct FmdScanJob job;
	memset(&job, 0, sizeof job);
	job.log = &log_hook;
	job.begin = &begin_hook;
	job.finish = &finish_hook;

	job.flags = fmdsf_single | fmdsf_metadata;
	if (r_flag)
		job.flags |= fmdsf_recursive;
	int i;
	for (i = 0; i < argc; ++i) {
		job.location = argv[i];
		int res = fmd_scan(&job);
		if (res == -1)
			err(EX_OSERR, "%s", argv[i]);
		struct FmdFile *it;
		for (it = job.first_file; it; it = it->next)
			fmd_print_file(it, 1, stdout);
		fmd_free_chain(job.first_file);
		job.first_file = 0;
	}

	if (m_flag) {
		fprintf(stderr, "libfmd Metrics/Statistics:\n");
		fprintf(stderr, " * file open count %lu\n",
			(unsigned long)job.n_filopens);
		fprintf(stderr, " * directory open count %lu\n",
			(unsigned long)job.n_diropens);
		fprintf(stderr, " * physical reads count %lu\n",
			(unsigned long)job.n_physreads);
		fprintf(stderr, " * logical reads count %lu\n",
			(unsigned long)job.n_logreads);
		fprintf(stderr, " * physical reads volume %.3f MB\n",
			job.v_physreads / 1024.0 / 1024.0);
		fprintf(stderr, " * logical reads volume %.3f MB\n",
			job.v_logreads / 1024.0 / 1024.0);
		fprintf(stderr, " * cache hits count %lu\n",
			(unsigned long)job.n_cachehits);
		fprintf(stderr, " * cache misses count %lu\n",
			(unsigned long)job.n_cachemisses);
	}

	return 0;
}
