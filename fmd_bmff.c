#include "fmd_priv.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Set to 1 to enable tracing */
static const int fmd_media_trace = 0;

/* ISO/IEC-14496-12 or ISO base media file format (BMFF).
 *
 * Frames are called "Boxes", or "atoms", and are defined by unique
 * (32-bits long) type identifier and length. Some types of Boxes are
 * FullBoxes, and also contain 8-bits of version + 24-bits of flags */
struct FmdBmffBoxIterator {
	struct FmdFrameIterator base;

	/* Used when iterating over the contents of a Box (child boxes
	 * within); set to 0, filesize, initially */
	off_t start_offs, end_offs;

	/* Current Box offset (relative to |start_offs|), initially
	 * -1. Incremented with |frame_size| on each ..._next() */
	off_t offs;
	/* Relative offset to current Box' data; its (data's) length
	 * is kept in |frame.datalen|; also incremented by _skip() */
	off_t data_offs;

	/* A copy of current Box' boxtype; |frame.type| also points
	 * here and |frame.typelen| is set in ..._create() */
	uint8_t box_type[4];
	/* Current Box size, including header; used as an offset to
	 * position to the next Box */
	size_t box_size;
};
#define GET_BMFF(_iter)							\
	(struct FmdBmffBoxIterator*)((char*)(_iter) - offsetof (struct FmdBmffBoxIterator, base))

struct FmdBmffScanContext {
	/* Copy of ftyp' Box fields to keep a track of file's type:
	 * 'M4A ' for audio, 'M4V ', 'mp41' and 'mp42' for video */
	uint8_t major_brand[4];
	uint32_t minor_vers;

	uint8_t handler_type[4];
};
struct FmdBmffHandlerMap {
	/* Call |handler| to process the Box with |child| type when
	 * iterating |parent| type. Parent is \0\0\0\0 at root level
	 * (where ftyp and moov are). Last |handler| should be 0 */
	uint8_t parent[4], child[4];
	int (*handler)(struct FmdBmffScanContext *state,
		       struct FmdFrameIterator *box,
		       int depth,
		       const struct FmdBmffHandlerMap *map);
};

static int
fmdp_bmffit_next(struct FmdFrameIterator *iter)
{
	assert(iter);
	if (!iter)
		return (errno = EINVAL), -1;

	struct FmdBmffBoxIterator *bmfit = GET_BMFF(iter);

	/* Move |offs| to start of next Box */
	if (bmfit->offs >= 0) {
		assert(bmfit->box_size);
		bmfit->offs += bmfit->box_size;
	} else {		/* 1st invocation of _next() */
		bmfit->offs = 0;
	}
	off_t absoffs = bmfit->start_offs + bmfit->offs;
	if (absoffs + 8 > bmfit->end_offs)
		return 0;	/* end-of-file */

	off_t payload_offs = 8;	/* Relative to |absoffs| */
	const uint8_t *p = iter->stream->get(iter->stream, absoffs, 8);
	if (!p)
		return -1;	/* Not within bounds */
	bmfit->box_size = fmdp_get_bits_be(p, 0, 32);
	const uint8_t *box_type = p + 4;
	if (bmfit->box_size == 0) {
		/* Box extends to the end of file */
		bmfit->box_size = bmfit->end_offs - absoffs;
	} else if (bmfit->box_size == 1) {
		/* Box size is 64-bit */
		payload_offs += 8;
		p = iter->stream->get(iter->stream, absoffs + 4, 12);
		if (!p)
			return -1; /* Not within bounds */
		bmfit->box_size = fmdp_get_bits_be(p, 0, 64);
		box_type = p + 8;
	}

	bmfit->data_offs = bmfit->offs + payload_offs;
	bmfit->base.data = 0;
	bmfit->base.datalen = bmfit->box_size - payload_offs;
	memcpy(bmfit->box_type, box_type, 4);

	return 1;
}

static int
fmdp_bmffit_read(struct FmdFrameIterator *iter)
{
	assert(iter);
	if (!iter)
		return (errno = EINVAL), -1;

	struct FmdBmffBoxIterator *bmfit = GET_BMFF(iter);
	off_t absoffs = bmfit->start_offs + bmfit->data_offs;
	size_t len = bmfit->base.datalen;
	if (absoffs + (off_t)len > bmfit->end_offs) {
		FMDP_X(ERANGE);
		return (errno = ERANGE), -1;
	}

	bmfit->base.data = iter->stream->get(iter->stream, absoffs, len);
	return bmfit->base.data ? 0 : -1;
}

static const uint8_t*
fmdp_bmffit_get(struct FmdFrameIterator *iter,
		off_t offs, size_t len)
{
	assert(iter);
	assert(offs >= 0);
	assert(offs < (off_t)iter->datalen);
	assert(len > 0);
	assert(offs + len <= iter->datalen);
	if (!iter)
		return (errno = EINVAL), (void*)0;

	struct FmdBmffBoxIterator *bmfit = GET_BMFF(iter);
	/* XXX: is size_t at least as-large, as off_t? */
	if (offs + len > bmfit->base.datalen) {
		FMDP_X(ERANGE);
		return (errno = ERANGE), (void*)0;
	}

	bmfit->base.data = 0;	/* Invalidate, as promised */
	off_t absoffs = bmfit->start_offs + bmfit->data_offs + offs;
	return iter->stream->get(iter->stream, absoffs, len);
}

/* Skip |off| bytes of data in |iter|. Currently this is a hack and is
 * not accessible via |FmdFrameIterator| interface */
static int
fmdp_bmffit_skip(struct FmdFrameIterator *iter,
		 off_t off)
{
	assert(iter);
	assert(off > 0);
	if (!iter || off <= 0)
		return (errno = EINVAL), -1;

	struct FmdBmffBoxIterator *bmfit = GET_BMFF(iter);
	if (off < (off_t)iter->datalen) {
		bmfit->data_offs += off;
		iter->datalen -= off;
		return 0;
	} else
		return (errno = ERANGE), -1;
}

static void
fmdp_bmffit_free(struct FmdFrameIterator *iter)
{
	assert(iter);
	if (!iter)
		return;

	struct FmdBmffBoxIterator *bmfit = GET_BMFF(iter);
	free(bmfit);
}


/* Bounded when |start_offs| and |end_offs| are also provided */
static struct FmdFrameIterator*
fmdp_bmffit_create(struct FmdStream *stream,
		   off_t start_offs, off_t end_offs)
{
	assert(stream);
	if (!stream)
		return (errno = EINVAL), (void*)0;

	struct FmdBmffBoxIterator *bmfit =
		(struct FmdBmffBoxIterator*)calloc(1, sizeof *bmfit);
	if (!bmfit)
		return 0;

	bmfit->base.next = &fmdp_bmffit_next;
	bmfit->base.read = &fmdp_bmffit_read;
	bmfit->base.get = &fmdp_bmffit_get;
	bmfit->base.free = &fmdp_bmffit_free;

	bmfit->base.stream = stream;

	/* Those two are const: box type is always 4 bytes; bytes in
	 * |bmfit->box_type| are changed from ..._next() */
	bmfit->base.type = bmfit->box_type;
	bmfit->base.typelen = 4;

	if (start_offs || end_offs) {
		assert(start_offs > 0);
		assert(end_offs > 0);
		assert(end_offs > start_offs);
		bmfit->start_offs = start_offs;
		bmfit->end_offs = end_offs;
	} else {
		bmfit->end_offs = stream->file->stat.st_size;
	}
	bmfit->offs = -1;   /* Signal _next() to start from 1st Box */

	return &bmfit->base;
}


/* Creates a new iterator to go over current Box */
static struct FmdFrameIterator*
fmdp_bmffit_create_framed(struct FmdBmffBoxIterator *bmfit)
{
	assert(bmfit);
	const off_t absoffs = bmfit->start_offs + bmfit->data_offs;
	return fmdp_bmffit_create(bmfit->base.stream,
				  absoffs, absoffs + bmfit->base.datalen);
}


/* fmdp_bmff_check_ family of helper functions verifies various
 * properties of given |iter|ator. Upon test failure a fmdlt_format
 * message is generated and non-zero value is returned */
/* Verifies that FullBox's version is zero */
static int
fmdp_bmff_check_vers0(struct FmdFrameIterator *iter)
{
	assert(iter);
	struct FmdScanJob *job = iter->stream->job;
	const uint8_t *p = iter->get(iter, 0, 4);
	if (!p) {
		job->log(job, iter->stream->file->path, fmdlt_oserr,
			 "%s(%s): %s", "read", iter->stream->file->path,
			 strerror(errno));
		return 0;
	}

	if (p[0] != 0) {
		const uint8_t *t = iter->type;
		job->log(job, iter->stream->file->path, fmdlt_format,
			 "format(%s): '%c%c%c%c' box, vers %u is unsupported",
			 iter->stream->file->path,
			 isprint((unsigned)t[0]) ? t[0] : '?',
			 isprint((unsigned)t[1]) ? t[1] : '?',
			 isprint((unsigned)t[2]) ? t[2] : '?',
			 isprint((unsigned)t[3]) ? t[3] : '?',
			 (unsigned)p[0]);
		return (errno = EPROTONOSUPPORT), 0;
	}
	return 1;
}

/* Verifies datalen is in [minsz, maxsz] and multiple to multto */
static int
fmdp_bmff_check_datalen(struct FmdFrameIterator *iter,
			size_t minsz, size_t maxsz, size_t multto)
{
	assert(iter);
	struct FmdScanJob *job = iter->stream->job;
	size_t n = iter->datalen;
	if (n < minsz || n > maxsz || (n % multto) != 0) {
		const uint8_t *t = iter->type;
		job->log(job, iter->stream->file->path, fmdlt_format,
			 "format(%s): '%c%c%c%c' box len %u not in [%u, %u] or multiple to %u",
			 iter->stream->file->path,
			 isprint((unsigned)t[0]) ? t[0] : '?',
			 isprint((unsigned)t[1]) ? t[1] : '?',
			 isprint((unsigned)t[2]) ? t[2] : '?',
			 isprint((unsigned)t[3]) ? t[3] : '?',
			 (unsigned)minsz, (unsigned)maxsz, (unsigned)multto);
		return (errno = EPROTONOSUPPORT), 0;
	}
	return 1;
}

/* Reads data and returns non-zero if succeeds. On failure generates
 * fmdlt_oserr and returns zero */
static int
fmdp_bmff_readdata(struct FmdFrameIterator *iter)
{
	assert(iter);
	if (iter->read(iter) != 0) {
		struct FmdScanJob *job = iter->stream->job;
		job->log(job, iter->stream->file->path, fmdlt_oserr,
			 "%s(%s): %s", "read", iter->stream->file->path,
			 strerror(errno));
		return 0;
	}
	return 1;
}


static int
fmdp_bmff_trace_iter(struct FmdFrameIterator *iter,
		     int depth)
{
	assert(iter);

	struct FmdBmffBoxIterator *bmfit = GET_BMFF(iter);
	struct FmdScanJob *job = iter->stream->job;
	while (iter->next(iter) == 1) {
		assert(!iter->data);
		if (fmd_media_trace)
			job->log(job, iter->stream->file->path, fmdlt_trace,
				 "%*s%c%c%c%c/%u %u + %u",
				 depth * 2, "",
				 isprint(iter->type[0]) ? iter->type[0] : '.',
				 isprint(iter->type[1]) ? iter->type[1] : '.',
				 isprint(iter->type[2]) ? iter->type[2] : '.',
				 isprint(iter->type[3]) ? iter->type[3] : '.',
				 (unsigned)iter->typelen,
				 (unsigned)(bmfit->start_offs +
					    bmfit->data_offs),
				 (unsigned)iter->datalen);

		/* Dive into Boxes known to contain child Boxes */
		if (!memcmp(iter->type, "moov", 4) ||
		    !memcmp(iter->type, "trak", 4) ||
		    !memcmp(iter->type, "mdia", 4) ||
		    !memcmp(iter->type, "minf", 4) ||
		    !memcmp(iter->type, "stbl", 4) ||
		    !memcmp(iter->type, "udta", 4)) {
			struct FmdFrameIterator *inner =
				fmdp_bmffit_create_framed(bmfit);
			if (inner) {
				fmdp_bmff_trace_iter(inner, depth + 1);
				inner->free(inner);
			}
		}
	}
	return 0;
}


/* Iterate children of given |box| and process children according the
 * to hierarchy defined with given |map| */
static int
fmdp_bmff_iterate_children(struct FmdBmffScanContext *ctx,
			   struct FmdFrameIterator *box,
			   int depth,
			   const struct FmdBmffHandlerMap *map)
{
	assert(ctx);
	assert(box);
	assert(map);
	if (!ctx || !box || !map)
		return (errno = EINVAL), -1;

	/* Make a copy of current type that is taken for |parent| type
	 * in |map| */
	struct FmdScanJob *job = box->stream->job;
	uint8_t currtype[4];
	memcpy(currtype, box->type, 4);
	if (fmd_media_trace)
		job->log(job, box->stream->file->path, fmdlt_trace,
			 "%*siterating '%4.4s'", depth * 2, "", currtype);

	/* Iterate child Boxes and search for matching handlers
	 * amongst |map| entries */
	int res = 0;
	struct FmdBmffBoxIterator *bmfit = GET_BMFF(box);
	struct FmdFrameIterator *iter = fmdp_bmffit_create_framed(bmfit);
	while (iter->next(iter) == 1) {
		const struct FmdBmffHandlerMap *mit;
		for (mit = map; mit->handler; ++mit) {
			if (!memcmp(mit->parent, currtype, 4) &&
			    !memcmp(mit->child, iter->type, 4)) {
				res = mit->handler(ctx, iter, depth + 1, map);
				if (res != 0)
					break;
			}
		}
	}
	iter->free(iter);
	return res;
}


static int
fmdp_bmff_do_ftyp(struct FmdBmffScanContext *ctx,
		  struct FmdFrameIterator *iter,
		  int depth,
		  const struct FmdBmffHandlerMap *map)
{
	assert(ctx);
	assert(iter);
	(void)depth;
	assert(map); (void)map;
	if (!ctx || !iter)
		return (errno = EINVAL), -1;

	/* NOTE: no def upper limit on ftyp' size, but practical */
	if (!fmdp_bmff_check_datalen(iter, 8, 160, 4) ||
	    !fmdp_bmff_readdata(iter))
		return -1;

	/* In a way ftyp Box identifies file type */
	assert(iter->data);
	memcpy(ctx->major_brand, iter->data, 4);
	ctx->minor_vers = fmdp_get_bits_be(iter->data, 32, 32);
	if (fmd_media_trace) {
		struct FmdScanJob *job = iter->stream->job;
		job->log(job, iter->stream->file->path,
			 fmdlt_trace,
			 "%*sftyp is '%.4s', vers %u",
			 depth * 2, "", ctx->major_brand,
			 (unsigned)ctx->minor_vers);
	}
	return 0;
}


static int
fmdp_bmff_do_moov_mvhd(struct FmdBmffScanContext *ctx,
		       struct FmdFrameIterator *iter,
		       int depth,
		       const struct FmdBmffHandlerMap *map)
{
	assert(ctx);
	assert(iter);
	(void)depth;
	assert(map); (void)map;
	if (!ctx || !iter)
		return (errno = EINVAL), -1;

	if (!fmdp_bmff_check_datalen(iter, 25 * 4, 28 * 4, 4) ||
	    !fmdp_bmff_readdata(iter))
		return -1;
	assert(iter->data);
	const uint8_t vers = iter->data[0];
	if (vers > 1) {
		struct FmdScanJob *job = iter->stream->job;
		job->log(job, iter->stream->file->path, fmdlt_format,
			 "format(%s): 'mvhd' vers %u is unsupported",
			 iter->stream->file->path, (unsigned)vers);
		return 0;
	}

	const uint8_t *p = iter->data + 4; /* skip vers & flags */
	long timescale, units;
	if (vers == 0) {	/* 32-bit time & duration */
		timescale = fmdp_get_bits_be(p, 2 * 32, 32);
		units = fmdp_get_bits_be(p, 3 * 32, 32);
	} else {		/* 64-bit time & duration */
		timescale = fmdp_get_bits_be(p, 2 * 64, 32);
		units = fmdp_get_bits_be(p, 2 * 64 + 32, 64);
	}
	if (units > 0 && timescale > 0) {
		double duration = (double)units / (double)timescale;
		return fmdp_add_frac(iter->stream->file, fmdet_duration,
				     duration);
	} else {
		struct FmdScanJob *job = iter->stream->job;
		job->log(job, iter->stream->file->path, fmdlt_format,
			 "format(%s): 'mvhd' w/ zero timescale",
			 iter->stream->file->path);
		return 0;
	}
}


static int
fmdp_bmff_do_meta_hdlr(struct FmdBmffScanContext *ctx,
		       struct FmdFrameIterator *iter,
		       int depth,
		       const struct FmdBmffHandlerMap *map)
{
	assert(ctx);
	assert(iter);
	(void)depth;
	assert(map); (void)map;
	if (!ctx || !iter)
		return (errno = EINVAL), -1;

	if (!fmdp_bmff_check_vers0(iter) ||
	    !fmdp_bmff_check_datalen(iter, 18, 180, 1) ||
	    !fmdp_bmff_readdata(iter))
		return -1;

	assert(iter->data);
	memcpy(ctx->handler_type, iter->data + 8, 4);
	if (fmd_media_trace) {
		struct FmdScanJob *job = iter->stream->job;
		job->log(job, iter->stream->file->path, fmdlt_trace,
			 "%*shandler_type is '%.4s'", depth * 2, "",
			 ctx->handler_type);
	}
	return 0;

}


static int
fmdp_bmff_do_md_field(uint8_t fieldid[4],
		      struct FmdFrameIterator *iter,
		      int depth)
{
	assert(fieldid);
	assert(iter);

	const uint8_t *p = iter->get(iter, 0, 8);
	if (!p)
		return -1;
	/* Well-Known Types: 0 (implied), 1 UTF-8, 2 UTF-16, ... */
	const uint32_t typeid = fmdp_get_bits_be(p, 0, 32);
	const uint32_t localeid = fmdp_get_bits_be(p, 32, 32);
	const size_t value_len = iter->datalen - 8;

	if (fmd_media_trace) {
		struct FmdScanJob *job = iter->stream->job;
		/* XXX: Wouldn't work well for integers */
		job->log(job, iter->stream->file->path,
			 fmdlt_trace,
			 "%*smd '%4.4s' (%d/%d) #%d",
			 depth * 2, "", fieldid, (int)typeid, (int)localeid,
			 (int)value_len);
	}

	static const struct FmdToken text_fields[] = {
		/* XXX: more */
		{ "\251nam", fmdet_title },
		{ "\251alb", fmdet_album },
		{ "aART", fmdet_artist },
		{ "\251ART", fmdet_performer },
		{ "\251too", fmdet_creator },
		{ "\251cmt", fmdet_description },
		{ "desc", fmdet_description },
		{ 0, 0 }
	};
	int t = fmdp_match_token_exact((const char*)fieldid, 4, text_fields);
	if (t != -1) {
		if (!value_len)
			return 0;
		if (iter->read(iter) == -1)
			return -1;	/* Can't read frame data */
		assert(iter->data);
		const char *value = (const char*)iter->data + 8;
		return fmdp_add_text(iter->stream->file, t, value, value_len);
	}	

	static const struct FmdToken num_fields[] = {
		/* XXX: more */
		{ "trkn", fmdet_trackno },
		{ 0, 0 }
	};
	t = fmdp_match_token_exact((const char*)fieldid, 4, num_fields);
	if (t != -1) {
		if (value_len < 4)
			return 0;
		if (iter->read(iter) == -1)
			return -1;	/* Can't read frame data */
		assert(iter->data);
		const long value = fmdp_get_bits_be(iter->data, 64, 32);
		return fmdp_add_n(iter->stream->file, t, value);
	}

	return 0;
}


static int
fmdp_bmff_do_meta_ilst(struct FmdBmffScanContext *ctx,
		       struct FmdFrameIterator *iter,
		       int depth,
		       const struct FmdBmffHandlerMap *map)
{
	assert(ctx);
	assert(iter);
	(void)depth;
	assert(map); (void)map;
	if (!ctx || !iter)
		return (errno = EINVAL), -1;

	struct FmdBmffBoxIterator *bmfit = GET_BMFF(iter);
	struct FmdFrameIterator *field = fmdp_bmffit_create_framed(bmfit);
	if (!field) {
		FMDP_X(-1);
		return -1;
	}

	/* Each child of 'ilst' box is a metadata property that we
	 * seek. However the value of each list entry is in a child
	 * box of type 'data': ilst[name[data], artist[data]]. data
	 * contains 32-bit typeid, 32-bit localeid and data itself */
	int res = 0;
	while (res == 0 && field->next(field) == 1) {
		uint8_t fieldid[4];
		memcpy(fieldid, field->type, 4);

		struct FmdFrameIterator *data =
			fmdp_bmffit_create_framed(GET_BMFF(field));
		if (!data) {
			FMDP_X(-1);
			res = -1;
			goto done;
		}
		if (data->next(data) == 1 &&
		    !memcmp(data->type, "data", 4)) {
			res = fmdp_bmff_do_md_field(fieldid, data, depth);
		}

		data->free(data);
	}

done:
	field->free(field);
	return res;
}


static int
fmdp_bmff_do_meta(struct FmdBmffScanContext *ctx,
		  struct FmdFrameIterator *iter,
		  int depth,
		  const struct FmdBmffHandlerMap *map)
{
	assert(ctx);
	assert(iter);
	(void)depth;
	assert(map); (void)map;
	if (!ctx || !iter)
		return (errno = EINVAL), -1;

	/* We only "speak" version 0, flags 0 */
	struct FmdScanJob *job = iter->stream->job;
	const uint8_t *p = iter->get(iter, 0, 4); /* vers & flags */
	if (!p) {
		job->log(job, iter->stream->file->path, fmdlt_oserr,
			 "%s(%s): %s", "read", iter->stream->file->path,
			 strerror(errno));
		FMDP_X(-1);
		return -1;
	}
	if (p[0] != 0 || p[1] != 0 || p[2] != 0 || p[3] != 0) {
		job->log(job, iter->stream->file->path, fmdlt_format,
			 "format(%s): meta Box, vers %u, flags %u unsupported",
			 iter->stream->file->path, (unsigned)p[0],
			 (unsigned)fmdp_get_bits_be(p, 8, 24));
		FMDP_X(-1);
		return (errno = EPROTONOSUPPORT), -1;
	}

	/* Skip over version & flags and iterate over child boxes */
	int res = fmdp_bmffit_skip(iter, 4);
	if (res != 0) {
		FMDP_X(-1);
		return -1;
	}

	static const struct FmdBmffHandlerMap handlermap[] = {
		{ { 'm', 'e', 't', 'a' }, { 'h', 'd', 'l', 'r' },
		  &fmdp_bmff_do_meta_hdlr },
		{ { 'm', 'e', 't', 'a' }, { 'i', 'l', 's', 't' },
		  &fmdp_bmff_do_meta_ilst },
		{ { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, 0 } /* term */
	};
	res = fmdp_bmff_iterate_children(ctx, iter, depth, handlermap);
	return res;
}


int
fmdp_do_bmff(struct FmdStream *stream)
{
	assert(stream);
	static const struct FmdBmffHandlerMap handlermap[] = {
		{ { 0, 0, 0, 0 }, { 'f', 't', 'y', 'p' },
		  &fmdp_bmff_do_ftyp },
		{ { 0, 0, 0, 0 }, { 'm', 'o', 'o', 'v' },
		  &fmdp_bmff_iterate_children },
		{ { 'm', 'o', 'o', 'v' }, { 'm', 'v', 'h', 'd' },
		  &fmdp_bmff_do_moov_mvhd },
		{ { 'm', 'o', 'o', 'v' }, { 'u', 'd', 't', 'a' },
		  &fmdp_bmff_iterate_children },
		{ { 'u', 'd', 't', 'a' }, { 'm', 'e', 't', 'a' },
		  &fmdp_bmff_do_meta },
		{ { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, 0 } /* term */
	};

	struct FmdFrameIterator *iter;

#if defined (_DEBUG) || 1
	iter = fmdp_bmffit_create(stream, 0, 0);
	if (iter) {
		fmdp_bmff_trace_iter(iter, 0);
		iter->free(iter);
	}
#endif

	struct FmdBmffScanContext ctx;
	memset(&ctx, 0, sizeof ctx);

	iter = fmdp_bmffit_create(stream, 0, 0);
	if (!iter)
		return -1;

	/* Unofficial details for (some) Quicktime atoms:
	 * http://atomicparsley.sourceforge.net/mpeg-4files.html and
	 * https://infohost.nmt.edu/~john/scans/d300/old-exiftool/html/TagNames/QuickTime.html#ImageDesc */
	/* Official documentation for Quicktime metadata atoms:
	 * https://developer.apple.com/library/archive/documentation/QuickTime/QTFF/Metadata/Metadata.html#//apple_ref/doc/uid/TP40000939-CH1-SW1 */
	int res = fmdp_bmff_iterate_children(&ctx, iter, /*depth*/0, handlermap);
	iter->free(iter);

	if (res == 0) {
		if (!memcmp(ctx.major_brand, "M4V ", 4) ||
		    !memcmp(ctx.major_brand, "mp41", 4) ||
		    !memcmp(ctx.major_brand, "mp42", 4)) {
			stream->file->filetype = fmdft_video;
			stream->file->mimetype = "video/mp4";
		} else if (!memcmp(ctx.major_brand, "M4A ", 4)) {
			stream->file->filetype = fmdft_audio;
			stream->file->mimetype = "audio/mp4"; /* ??? */
		} else {
			stream->file->filetype = fmdft_media;
		}
	}

	return res;
}
