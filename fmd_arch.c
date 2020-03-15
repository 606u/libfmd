#include "fmd_priv.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <archive.h>
#include <archive_entry.h>

static const int fmd_arch_trace = 1;

struct FmdArchState {
	struct FmdStream *stream;
	off_t size, off;
};

static la_ssize_t
fmdp_arch_read_callback(struct archive *a, void *udata, const void **buf)
{
	struct FmdArchState *ast = (struct FmdArchState*)udata;
	off_t sz = FMDP_READ_PAGE_SZ / 2;
	if (ast->off + sz > ast->size) {
		sz = ast->size - ast->off; /* Cannot read that much */
		if (!sz)
			return 0; /* end-of-file */
	}
	const uint8_t *p = ast->stream->get(ast->stream, ast->off, sz);
	if (p) {
		ast->off += sz;
		*buf = p;
		return sz;
	}
	archive_set_error(a, errno, "%s", strerror(errno));
	return -1;
}

static la_int64_t
fmdp_arch_skip_callback(struct archive *a, void *udata, off_t off)
{
	(void)a;
	struct FmdArchState *ast = (struct FmdArchState*)udata;
	if (ast->off + off > ast->size)
		off = ast->size - ast->off; /* Cannot seek that much */
	ast->off += off;
	return off;
}

static int
fmdp_arch_close_callback(struct archive *a, void *udata)
{
	(void)a; (void)udata;
	return ARCHIVE_OK;
}


int
fmdp_do_arch(struct FmdStream *stream)
{
	assert(stream);

	struct FmdScanJob *job = stream->job;
	int rv = 0;

	struct FmdArchState ast;
	ast.stream = stream;
	ast.size = stream->size(stream);
	ast.off = 0;

	if (fmd_arch_trace)
		job->log(job, stream->file->path, fmdlt_trace,
			 "testing if '%s' is an archive", stream->file->path);
	struct archive *a = archive_read_new();
	if (!a)
		return (errno = ENOMEM), -1;

	archive_read_support_filter_all(a);
	archive_read_support_format_all(a);
	rv = archive_read_open2(a, &ast, /*open:*/0, &fmdp_arch_read_callback,
				&fmdp_arch_skip_callback, &fmdp_arch_close_callback);
	if (rv == ARCHIVE_OK) {
		if (fmd_arch_trace)
			job->log(job, stream->file->path, fmdlt_trace,
				 "archive '%s' opened", stream->file->path);
		struct archive_entry *entry;
		while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
			if (fmd_arch_trace)
				job->log(job, stream->file->path, fmdlt_trace,
					 " -> %s",archive_entry_pathname(entry));
		}
		rv = 0;
	} else {
		if (fmd_arch_trace)
			job->log(job, stream->file->path, fmdlt_trace,
				 "not an archive");
		errno = EPROTONOSUPPORT;
		rv = -1;
	}
	archive_read_free(a);

	return rv;
}
