#include "fmd_priv.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const int fmd_exif_trace = 0;

/* JPEG segment iterator */
struct FmdJpegIterator {
	struct FmdFrameIterator base;

	/* Current Box offset (relative to |start_offs|), initially
	 * -1. Incremented with |frame_size| on each ..._next() */
	off_t offs;

	/* The value of the byte that follows 0xff. |frame.type|
	 * points here and |frame.typelen| is set in ..._create() */
	uint8_t marker;
	/* Current segment size, including header; used as an offset
	 * to position to the next segment */
	off_t seg_size;
};
#define GET_JPEG(_iter)							\
	(struct FmdJpegIterator*)((char*)(_iter) - offsetof (struct FmdJpegIterator, base))

static int
fmdp_jpegit_next(struct FmdFrameIterator *iter)
{
	assert(iter);
	if (!iter)
		return (errno = EINVAL), -1;

	struct FmdJpegIterator *jpgit = GET_JPEG(iter);
	struct FmdScanJob *job = iter->stream->job;

	/* Move |offs| to start of next Box */
	if (jpgit->offs >= 0) {
		jpgit->offs += jpgit->seg_size;
	} else {		/* 1st invocation of _next() */
		jpgit->offs = 0;
	}
	const uint8_t *p = iter->stream->get(iter->stream, jpgit->offs, 4);
	if (!p)
		return -1;	/* Not within bounds */
	if (p[0] != 0xff) {
		errno = EPROTONOSUPPORT;
		job->log(job, iter->stream->file->path, fmdlt_format,
			 "got 0x%02x, instead of 0xff at '%s', offs %u",
			 p[0], iter->stream->file->path, (unsigned)jpgit->offs);
		return -1;
	}

	jpgit->marker = p[1];
	if (p[1] != 0xd8 && p[2] != 0xd9) {
		long len = fmdp_get_bits_be(p, 16, 16);
		if (len < 2) {
			errno = EPROTONOSUPPORT;
			job->log(job, iter->stream->file->path, fmdlt_format,
				 "segment len %u < 2 at '%s', offs %u",
				 (unsigned)len, iter->stream->file->path,
				 (unsigned)jpgit->offs);
			return -1;
		}
		jpgit->base.datalen = len - 2;
		jpgit->seg_size = 2 + len;
	} else {	    /* SOI/EOI, start/end of image, no data */
		jpgit->base.datalen = 0;
		jpgit->seg_size = 2;
	}
	jpgit->base.data = 0;
	return 1;
}

static const uint8_t*
fmdp_jpegit_get(struct FmdFrameIterator *iter,
		off_t offs, size_t len)
{
	assert(iter);
	assert(offs >= 0);
	assert(offs < (off_t)iter->datalen);
	assert(len > 0);
	assert(offs + len <= iter->datalen);
	if (!iter)
		return (errno = EINVAL), (void*)0;

	struct FmdJpegIterator *jpgit = GET_JPEG(iter);
	/* XXX: is size_t at least as-large, as off_t? */
	if (offs + len > jpgit->base.datalen) {
		FMDP_X(ERANGE);
		return (errno = ERANGE), (void*)0;
	}

	jpgit->base.data = 0;	/* Invalidate, as promised */
	return iter->stream->get(iter->stream, jpgit->offs + offs, len);
}

static int
fmdp_jpegit_read(struct FmdFrameIterator *iter)
{
	assert(iter);
	if (!iter)
		return (errno = EINVAL), -1;

	struct FmdJpegIterator *jpgit = GET_JPEG(iter);
	assert(jpgit->offs >= 0);  /* _next() never called */
	assert(iter->datalen > 0); /* No data to read */
	if (jpgit->offs >= 0 && iter->datalen) {
		iter->data = fmdp_jpegit_get(iter, 0, iter->datalen);
		return iter->data ? 0 : -1;
	}
	return (errno = EOPNOTSUPP), -1;
}

static void
fmdp_jpegit_free(struct FmdFrameIterator *iter)
{
	assert(iter);
	if (!iter)
		return;

	struct FmdJpegIterator *jpgit = GET_JPEG(iter);
	free(jpgit);
}


static struct FmdFrameIterator*
fmdp_jpegit_create(struct FmdStream *stream)
{
	assert(stream);
	if (!stream)
		return (errno = EINVAL), (void*)0;

	struct FmdJpegIterator *jpgit =
		(struct FmdJpegIterator*)calloc(1, sizeof *jpgit);
	if (!jpgit)
		return 0;

	jpgit->base.next = &fmdp_jpegit_next;
	jpgit->base.read = &fmdp_jpegit_read;
	jpgit->base.get = &fmdp_jpegit_get;
	jpgit->base.free = &fmdp_jpegit_free;

	jpgit->base.stream = stream;

	/* Those two are const: marker is always 1 byte; marker itself
	 * is assigned from ..._next() */
	jpgit->base.type = &jpgit->marker;
	jpgit->base.typelen = 1;
	jpgit->offs = -1;   /* Signal _next() to start from 1st Box */

	return &jpgit->base;
}


/* Returns FmdStream to access the payload where |iter| is */
static struct FmdStream*
fmdp_exif_embedded_tiff_stream(struct FmdFrameIterator *iter)
{
	assert(iter);
	if (!iter)
		return 0;

	struct FmdJpegIterator *jpgit = GET_JPEG(iter);
	assert(jpgit->offs >= 0);  /* _next() never called */
	assert(iter->datalen > 0); /* No data to read */
	off_t off = /* ffe1 + iter size */ 4 + /* Exif\0\0 */ 6;
	if (jpgit->offs >= 0 && (off_t)iter->datalen > off)
		return fmdp_ranged_stream_create(iter->stream,
						 jpgit->offs + off,
						 jpgit->base.datalen - off);
	return (errno = EOPNOTSUPP), (void*)0;
}


int
fmdp_do_exif(struct FmdStream *stream)
{
	assert(stream);

	struct FmdFrameIterator *iter = fmdp_jpegit_create(stream);
	if (!iter)
		return -1;

	/* XXX: include picture's pixel width & height */
	struct FmdScanJob *job = stream->job;
	int rv = 0;
	while (iter->next(iter) == 1) {
		assert(iter->type);
		assert(iter->typelen == 1);
		assert(!iter->data);
		if (fmd_exif_trace)
			job->log(job, stream->file->path, fmdlt_trace,
				 "marker 0x%02x, len %u",
				 iter->type[0], (unsigned)iter->datalen);
		if (iter->type[0] == 0xe1) {
			/* APP1 marker, payload is TIFF ExifIFD */
			struct FmdStream *exifstr =
				fmdp_exif_embedded_tiff_stream(iter);
			if (exifstr) {
				rv = fmdp_do_tiff(exifstr);
				exifstr->close(exifstr);
				if (rv == 0)
					stream->file->mimetype = "image/jpeg";
			}
		}
	}
	iter->free(iter);

	return rv;
}
