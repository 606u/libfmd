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

	struct archive *a = archive_read_new();
	if (!a)
		return (errno = ENOMEM), -1;

	archive_read_support_filter_all(a);
	archive_read_support_format_all(a);
	rv = archive_read_open2(a, &ast, /*open:*/0, &fmdp_arch_read_callback,
				&fmdp_arch_skip_callback, &fmdp_arch_close_callback);
	if (rv == ARCHIVE_OK) {
		/* TODO: read mime-type from libarchive(3) */
		stream->file->filetype = fmdft_archive;
		if (fmd_arch_trace)
			job->log(job, stream->file->path, fmdlt_trace,
				 "archive '%s' opened with %d filter(s)",
				 stream->file->path, archive_filter_count(a));

		struct FmdFile *children = 0, *tail = 0;
		struct archive_entry *entry;
		while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
			const char *epath = archive_entry_pathname(entry);

			if (fmd_arch_trace)
				job->log(job, stream->file->path, fmdlt_trace,
					 " -> %s", epath);
			snprintf(job->priv->scratch, sizeof (job->priv->scratch),
				 "%s/%s", stream->file->path, epath);
			struct FmdFile *file = fmdp_file_new(job, job->priv->scratch);
			if (!file) {
				rv = -1;
				break;
			}
			file->stat = *archive_entry_stat(entry);

			if (tail)
				tail->next = file;
			else
				children = file;
			tail = file;
		}

		if (children) {
			/* Put children right after archive they're in */
			assert(tail);
			tail->next = stream->file->next;
			stream->file->next = children;
		}

		rv = 0;
	} else {
		errno = EPROTONOSUPPORT;
		rv = -1;
	}
	archive_read_free(a);

	return rv;
}
