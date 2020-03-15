#include "fmd_priv.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <archive.h>
#include <archive_entry.h>

static const int fmd_arch_trace = 0;

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


/* Forward-only FmdStream on top of libarchive(3) entry. Best used
 * with FmdCachedStream to provide *rudimentary* seeking support */
struct FmdArchStream {
	struct FmdStream base;

	struct archive *a;
	struct archive_entry *entry;
	off_t size, off;
	char buf[FMDP_READ_PAGE_SZ];
};
#define FMDP_GET_ASTR(_stream)			\
	(struct FmdArchStream*)((_stream) - offsetof(struct FmdArchStream, base))

static off_t
fmdp_arch_stream_size(struct FmdStream *stream)
{
	assert(stream);
	struct FmdArchStream *astr = FMDP_GET_ASTR(stream);
	return (off_t)archive_entry_size(astr->entry);
}

static const uint8_t*
fmdp_arch_stream_get(struct FmdStream *stream,
		     off_t offs, size_t len)
{
	assert(stream);
	assert(len);
	assert(len <= FMDP_READ_PAGE_SZ);

	struct FmdArchStream *astr = FMDP_GET_ASTR(stream);
	off_t skip = offs - astr->off;
	if (skip < 0) {
		/* Cannot seek backwards; that is what FmdCachedStream
		 * is for.  XXX: However skipped data is not cached,
		 * therefore archived ZIPs cannot be processed. */
		errno = ESPIPE;
		FMDP_XM(-1, "seek backwards %d in '%s' impossible",
			(int)skip, stream->file->path);
		return 0;
	}

	if (astr->off + (off_t)len > astr->size) {
		/* Avoid read after end-of-file */
		//assert(0);
		len = (size_t)(astr->size - astr->off);
	}

	/* No libarchive(3) API to seek, therefore read and discard */
	int rv;
	while (skip) {
		off_t l = skip > FMDP_READ_PAGE_SZ ? FMDP_READ_PAGE_SZ : skip;
		if ((rv = archive_read_data(astr->a, astr->buf, l)) != l) {
			len = l;
			goto err;
		}
		astr->off += l;
		skip -= l;
	}

	if ((rv = archive_read_data(astr->a, astr->buf, len)) == (int)len) {
		/* Success */
		astr->off += len;
		return (uint8_t*)astr->buf;
	}

err:
	stream->job->log(stream->job, stream->file->path, fmdlt_oserr,
			 "archive_read_data(%s, %lu + %lu): %d, %s",
			 stream->file->path, (unsigned long)astr->off,
			 (unsigned long)len, rv, archive_error_string(astr->a));
	errno = EIO;
	FMDP_X(-1);
	return 0;
}

static void
fmdp_arch_stream_close(struct FmdStream *stream)
{
	assert(stream);
	struct FmdArchStream *astr = FMDP_GET_ASTR(stream);
	/* Nothing to close */
	archive_read_data_skip(astr->a);
	free(astr);
}

static struct FmdStream*
fmdp_arch_stream(struct FmdScanJob *job, struct FmdFile *file,
		 struct archive *a, struct archive_entry *entry)
{
	assert(job);
	assert(file);
	assert(a);
	assert(entry);
	struct FmdArchStream *astr = (struct FmdArchStream*)calloc (1, sizeof (*astr));
	if (astr) {
		astr->base.size = fmdp_arch_stream_size;
		astr->base.get = &fmdp_arch_stream_get;
		astr->base.close = &fmdp_arch_stream_close;
		astr->base.job = job;
		astr->base.file = file;
		astr->a = a;
		astr->entry = entry;
		astr->size = (off_t)archive_entry_size(entry);
		astr->off = 0;
		return &astr->base;
	}
	return 0;
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

			if (!S_ISREG(file->stat.st_mode))
				/* Cannot probe non-files */
				goto skipprobe;
			struct FmdStream *uncached =
				fmdp_arch_stream(job, file, a, entry);
			if (uncached) {
				struct FmdStream *cached = fmdp_cache_stream(uncached);
				if (cached) {
					rv = fmdp_probe_stream(cached);
					cached->close(cached);
				} else {
					rv = fmdp_probe_stream(uncached);
					uncached->close(uncached);
				}
			} /* else cannot probe */

		skipprobe:
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
