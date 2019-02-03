#include "fmd_priv.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Documentation */
/* 1. https://www.awaresystems.be/imaging/tiff/specification/TIFF6.pdf */
/* 2. https://www.awaresystems.be/imaging/tiff.html */

/* Set to 1 to enable tracing */
static const int fmd_tiff_trace = 0;

enum FmdpTiffTagType {
	fmdp_ttt_width = 256,		  /* short/long */
	fmdp_ttt_height = 257,		  /* short/long */
	fmdp_ttt_bits_per_sample = 258,	  /* short */
	fmdp_ttt_photometr_interpr = 262, /* short */
	fmdp_ttt_docname = 269,		  /* ASCII */
	fmdp_ttt_description = 270,	  /* ASCII */
	fmdp_ttt_devicevendor = 271,	  /* ASCII */
	fmdp_ttt_devicemodel = 272,	  /* ASCII */
	fmdp_ttt_samples_per_pixel = 277, /* short */
	fmdp_ttt_pagename = 285,	  /* ASCII */
	fmdp_ttt_pageno = 297,		  /* N out of M, short(2) */
	fmdp_ttt_software = 305,	  /* ASCII */
	fmdp_ttt_datetime = 306, /* ASCII(20), "YYYY:MM:DD HH:MM:SS" */
	fmdp_ttt_artist = 315,	 /* ASCII */
	fmdp_ttt_hostcomp = 316, /* ASCII */
	fmdp_ttt_copyright = 33432,	     /* ASCII */
	fmdp_ttt_exif_exposure_time = 33434, /* rational */
	fmdp_ttt_exif_fnumber = 33437,	     /* rational */
	fmdp_ttt_exif_exposure_prog = 34850, /* short */
	fmdp_ttt_exififd = 34665,	 /* long, offs to ExifIFD */
	fmdp_ttt_gpsifd = 34853,	 /* long, offs to GpsIFD */
	fmdp_ttt_exif_iso_speed = 34855, /* short */
	fmdp_ttt_exif_focal_length = 37386,   /* rational */
	fmdp_ttt_exif_focal_length35 = 41989, /* rational */
	fmdp_ttt_interoperability = 40965,    /* long, offs */
};
enum FmdpTiffEntryType {
	fmdp_tet_byte = 1,	 /* 8-bit unsigned */
	fmdp_tet_ascii = 2,	 /* 7-bit ASCII with trailing zero */
	fmdp_tet_short = 3,	 /* 16-bit unsigned */
	fmdp_tet_long = 4,	 /* 32-bit unsigned */
	fmdp_tet_rational = 5,	 /* two longs: numerator &
				  * denominator */
	/* TIFF 6.0 added: */
	fmdp_tet_sbyte = 6,	 /* 8-bit signed twos-complement */
	fmdp_tet_undefined = 7,	 /* depends on field's definition */
	fmdp_tet_sshort = 8,	 /* 16-bit signed twos-complement */
	fmdp_tet_slong = 9,	 /* 32-bit signed twos-complement */
	fmdp_tet_srational = 10, /* two slongs: num & denom */
	fmdp_tet_float = 11,	 /* Single precision (4-byte) IEEE */
	fmdp_tet_double = 12,	 /* Double precision (8-byte) IEEE */
};
static const uint8_t fmdp_tiff_data_sz[13] = {
	/* Sizes for each of TIFF entry types, in bytes */
	0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8
};
/* Declares correct form of supported TIFF IFD entry tags: expected
 * data type, in form of or mask of (1 << FmdpTiffEntryType), and
 * valid count (if != 0), sorted ascending by tag */
#define FMDPX(_t) (1 << (fmdp_tet_##_t))
struct FmdpTiffEntryDecl {
	uint16_t tag;
	uint16_t typemask;
	uint32_t count;
};
static struct FmdpTiffEntryDecl fmdp_tiff_entries[] = {
	{ fmdp_ttt_width, FMDPX(short) | FMDPX(long), 1 },
	{ fmdp_ttt_height, FMDPX(short) | FMDPX(long), 1 },
	{ fmdp_ttt_bits_per_sample, FMDPX(short), 0 },
	{ fmdp_ttt_docname, FMDPX(ascii), 0 },
	{ fmdp_ttt_description, FMDPX(ascii), 0 },
	{ fmdp_ttt_devicevendor, FMDPX(ascii), 0 },
	{ fmdp_ttt_devicemodel, FMDPX(ascii), 0 },
	{ fmdp_ttt_samples_per_pixel, FMDPX(short), 1 },
	{ fmdp_ttt_software, FMDPX(ascii), 0 },
	{ fmdp_ttt_artist, FMDPX(ascii), 0 },
	{ fmdp_ttt_exif_exposure_time, FMDPX(rational), 1 },
	{ fmdp_ttt_exif_fnumber, FMDPX(rational), 1 },
	{ fmdp_ttt_exififd, FMDPX(long), 1 },
	{ fmdp_ttt_gpsifd, FMDPX(long), 1 },
	{ fmdp_ttt_exif_iso_speed, FMDPX(short), 0 },
	{ fmdp_ttt_exif_focal_length, FMDPX(rational), 1 },
	{ fmdp_ttt_exif_focal_length35, FMDPX(short), 1 },
};
static size_t fmdp_n_tiff_entries =
	sizeof (fmdp_tiff_entries) / sizeof (fmdp_tiff_entries[0]);
#undef FMDPX

struct FmdpTiffIfdEntry {
	/* Each entry either references a location in the TIFF file,
	 * where its value is written (via |offs|), or, if value is up
	 * to 4 octets, value is stored inline */

	uint16_t tag;		/* enum FmdpTiffTagType */
	uint16_t extref:1;	/* whether data is referenced
				 * (otherwise kept inline) */
	uint16_t type:15;	/* enum FmdpTiffEntryType */
	uint32_t count;		/* in units of |type| */

	union {
		uint8_t v_byte[4];
		char v_char[4];
		uint16_t v_short[2];
		uint32_t v_long;
		/* No other data type fits inline (apart from
		 * signed counterparts) */

		uint32_t offs;
	};	
};

struct FmdpTiffScanContext {
	struct FmdStream *stream;

	/* Fields that are defined fixed (like a single short or long)
	 * are kept as value. Other fields, that can be of variable
	 * size (like 1 to 3 shorts or ASCII strings) are kept as
	 * copies of IFD entries as they have to be explicitly read */
	uint32_t width, height;
	uint16_t samples_per_pixel;
	uint32_t exififd_offs, gpsifd_offs;
	struct FmdpTiffIfdEntry bits_per_sample;
	struct FmdpTiffIfdEntry docname;
	struct FmdpTiffIfdEntry description;
	struct FmdpTiffIfdEntry devicevendor;
	struct FmdpTiffIfdEntry devicemodel;
	struct FmdpTiffIfdEntry software;
	struct FmdpTiffIfdEntry artist;

	/* Exif IFD: */
	struct FmdpTiffIfdEntry exposure_time;
	struct FmdpTiffIfdEntry fnumber;
	struct FmdpTiffIfdEntry exposure_program;
	struct FmdpTiffIfdEntry iso_speed;
	struct FmdpTiffIfdEntry exif_version;
	struct FmdpTiffIfdEntry shutter_speed;
	struct FmdpTiffIfdEntry aperture;
	struct FmdpTiffIfdEntry subject_distance;
	struct FmdpTiffIfdEntry metering_mode;
	struct FmdpTiffIfdEntry light_source;
	struct FmdpTiffIfdEntry flash;
	struct FmdpTiffIfdEntry focal_length;
	struct FmdpTiffIfdEntry focal_length35;

	/* Returns |len| bits from |p| starting at |offs| (in bits) */
	long (*bits)(const uint8_t *p, size_t offs, size_t len);
};

typedef int (*FmdpTiffIfdEntryHook)(struct FmdpTiffScanContext *ctx,
				    const struct FmdpTiffIfdEntry *entry,
				    size_t ifd_index);

static int
fmdp_tiff_parse_ifd_entry(struct FmdpTiffScanContext *ctx,
			  const uint8_t p[12],
			  struct FmdpTiffIfdEntry *entry)
{
	assert(ctx);
	assert(p);
	assert(entry);

	struct FmdStream *stream = ctx->stream;
	struct FmdScanJob *job = stream->job;

	entry->tag = (uint16_t)ctx->bits(p, 0, 16);
	uint16_t type = (uint16_t)ctx->bits(p, 16, 16);
	if (type > 12) {
		job->log(job, stream->file->path, fmdlt_format,
			 "format(%s): TIFF IFD entry type %u (tag %u) is unsupported",
			 stream->file->path, type, entry->tag);
		return 0;
	}
	entry->type = type;
	entry->count = (uint32_t)ctx->bits(p, 32, 32);
	if (!entry->count) {
		job->log(job, stream->file->path, fmdlt_format,
			 "format(%s): TIFF IFD entry tag %u, type %u, zero count",
			 stream->file->path, entry->tag, type);
		return (errno = EPROTONOSUPPORT), -1;
	}

	size_t bytesz = entry->count * (size_t)fmdp_tiff_data_sz[type];
	if (bytesz <= 4) {
		entry->extref = 0;
		switch (type) {
		case fmdp_tet_byte:
			memcpy(entry->v_byte, p + 8, 4);
			break;
		case fmdp_tet_ascii:
			memcpy(entry->v_char, p + 8, 4);
			break;
		case fmdp_tet_short:
			entry->v_short[0] = ctx->bits(p, 64, 16);
			entry->v_short[1] = ctx->bits(p, 80, 16);
			break;
		case fmdp_tet_long:
			entry->v_long = (uint32_t)ctx->bits(p, 64, 32);
			break;
		}
	} else {
		entry->extref = 1;
		entry->offs = (uint32_t)ctx->bits(p, 64, 32);
		off_t endoffs = entry->offs + bytesz;
		if (endoffs > stream->size(stream)) {
			errno = EPROTONOSUPPORT;
			job->log(job, stream->file->path, fmdlt_format,
				 "format(%s): TIFF IFD entry tag %u, type %u, references after EOF, %u > %u",
				 stream->file->path,
				 entry->tag, entry->type,
				 (unsigned)endoffs,
				 (unsigned)stream->size(stream));
			return -1;
		}
	}
	return 1;
}


static int
fmdp_tiff_do_baseline_ifd(struct FmdpTiffScanContext *ctx,
			  const struct FmdpTiffIfdEntry *entry,
			  size_t ifd_index)
{
	assert(ctx);
	assert(entry);

	/* Don't know how to handle IFDs, apart from 1st one */
	if (ifd_index != 0)
		return 0;

	const uint32_t v = (entry->type == fmdp_tet_short ?
			    entry->v_short[0] : entry->v_long);

	switch (entry->tag) {
	case fmdp_ttt_width: ctx->width = v; break;
	case fmdp_ttt_height: ctx->height = v; break;
	case fmdp_ttt_bits_per_sample:
		ctx->bits_per_sample = *entry; break;
	case fmdp_ttt_docname: ctx->docname = *entry; break;
	case fmdp_ttt_description: ctx->description = *entry; break;
	case fmdp_ttt_devicevendor: ctx->devicevendor = *entry; break;
	case fmdp_ttt_devicemodel: ctx->devicemodel = *entry; break;
	case fmdp_ttt_software: ctx->software = *entry; break;
	case fmdp_ttt_artist: ctx->artist = *entry; break;
	case fmdp_ttt_samples_per_pixel:
		ctx->samples_per_pixel = (uint16_t)v; break;
	case fmdp_ttt_exififd: ctx->exififd_offs = v; break;
	case fmdp_ttt_gpsifd: ctx->gpsifd_offs = v; break;
	}
	return 0;
}


static int
fmdp_tiff_do_exififd(struct FmdpTiffScanContext *ctx,
		     const struct FmdpTiffIfdEntry *entry,
		     size_t ifd_index)
{
	assert(ctx);
	assert(entry);
	assert(!ifd_index);

	switch (entry->tag) {
	case fmdp_ttt_exif_exposure_time:
		ctx->exposure_time = *entry; break;
	case fmdp_ttt_exif_fnumber: ctx->fnumber = *entry; break;
	case fmdp_ttt_exif_iso_speed: ctx->iso_speed = *entry; break;
	case fmdp_ttt_exif_focal_length: ctx->focal_length = *entry; break;
	case fmdp_ttt_exif_focal_length35: ctx->focal_length35 = *entry; break;
	case fmdp_ttt_exif_exposure_prog:
		ctx->exposure_program = *entry; break;
	}
	return 0;
}


static int
fmdp_tiff_do_gpsifd(struct FmdpTiffScanContext *ctx,
		    const struct FmdpTiffIfdEntry *entry,
		    size_t ifd_index)
{
	assert(ctx);
	assert(entry);
	assert(!ifd_index);

	return 0;
}


/* |ifd_type| is 0 for regular IFDs (their index is in |ifd_index|),
 * fmdp_ttt_exififd for ExifIFD or fmdp_ttt_gpsifd for GpsIFD */
static int
fmdp_tiff_do_ifd(struct FmdpTiffScanContext *ctx,
		 uint32_t ifd_type,
		 size_t ifd_index,
		 off_t ifd_offs,
		 FmdpTiffIfdEntryHook hook)
{
	assert(ctx);
	assert(ifd_offs > 0);

	/* Get # of entries in the IFD (image file directory) */
	struct FmdStream *stream = ctx->stream;
	struct FmdScanJob *job = stream->job;

	if (ifd_offs < 8) {
		/* 8 bytes TIFF header: 4 bytes magic, 4 bytes offs */
		job->log(job, stream->file->path, fmdlt_format,
			 "format(%s): IFD cannot start at offs %u",
			 stream->file->path, (unsigned)ifd_offs);
		return (errno = EPROTONOSUPPORT), 1;
	}

	const uint8_t *p = stream->get(stream, ifd_offs, 2);
	if (!p)
		return -1;
	size_t entries = ctx->bits(p, 0, 16);
	if (entries <= 0) {
		errno = EPROTONOSUPPORT;
		job->log(job, stream->file->path, fmdlt_format,
			 "format(%s): TIFF IFD%u # entries is %d",
			 stream->file->path, (unsigned)ifd_index, entries);
		return -1;
	}

	/* Read whole IFD */
	size_t len = 2 + entries * 12 + 4;
	p = stream->get(stream, ifd_offs, len);
	if (!p)
		return -1;

	p += 2;
	if (fmd_tiff_trace)
		job->log(job, stream->file->path, fmdlt_trace,
			 "IFD%u: @ %u, type %u, %d entries, len %u:",
			 (unsigned)ifd_index, (unsigned)ifd_offs,
			 (unsigned)ifd_type, entries,
			 (unsigned)(entries * 12 + 2 + 4));
	unsigned i, di = 0;
	int past_tag = -1;
	struct FmdpTiffIfdEntry entry;
	for (i = 0; i < entries; ++i, p += 12) {
		int res = fmdp_tiff_parse_ifd_entry(ctx, p, &entry);
		if (res == -1)
			return -1;
		if (res != 1)
			/* Could be an unsupported entry */
			continue;

		if ((int)entry.tag <= past_tag) {
			/* QREF: The entries in an IFD must be sorted
			 * in ascending order by Tag. */
			job->log(job, stream->file->path, fmdlt_format,
				 "format(%s): IFD%u[%d] tag %d follows %d",
				 stream->file->path, (unsigned)ifd_index, i,
				 entry.tag, past_tag);
			return (errno = EPROTONOSUPPORT), 1;
		}

		if (fmd_tiff_trace) {
			if (entry.extref)
				job->log(job, stream->file->path, fmdlt_trace,
					 "IFD%u[%d] %d.%d, %u @ %u (0x%08x)",
					 (unsigned)ifd_index, i,
					 entry.tag, entry.type,
					 (unsigned)entry.count,
					 (unsigned)entry.offs,
					 (unsigned)entry.offs);
			else {
				switch (entry.type) {
				case fmdp_tet_short:
					job->log(job, stream->file->path,
						 fmdlt_trace,
						 "IFD%u[%d] %d.%d, %u",
						 (unsigned)ifd_index, i,
						 entry.tag, entry.type,
						 (unsigned)entry.v_short[0]);
					break;
				case fmdp_tet_long:
					job->log(job, stream->file->path,
						 fmdlt_trace,
						 "IFD%u[%d] %d.%d, %u",
						 (unsigned)ifd_index, i,
						 entry.tag, entry.type,
						 (unsigned)entry.v_long);
					break;
				default:
					job->log(job, stream->file->path,
						 fmdlt_trace,
						 "IFD%u[%d] %d.%d, %u",
						 (unsigned)ifd_index, i,
						 entry.tag, entry.type,
						 (unsigned)entry.count);
					break;
				}
			}
		}

		/* Entries in |fmdp_tiff_entries| are sorted by tag,
		 * same as entries in the IFD; merge scan */
		while (di < fmdp_n_tiff_entries &&
		       fmdp_tiff_entries[di].tag < entry.tag) {
			/* Make sure order is strictly incremental! */
			assert(!di || fmdp_tiff_entries[di - 1].tag < fmdp_tiff_entries[di].tag);
			++di;
		}
		const struct FmdpTiffEntryDecl *decl =
			di < fmdp_n_tiff_entries ? &fmdp_tiff_entries[di] : 0;
		if (!decl || decl->tag != entry.tag)
			/* We don't care about this IFD entry */
			continue;
			 
		if (!(decl->typemask & (1 << entry.type))) {
			job->log(job, stream->file->path, fmdlt_format,
				 "format(%s): IFD%u[%d].%d unexpected type %d",
				 stream->file->path,
				 (unsigned)ifd_index, i,
				 entry.type);
			continue;
		}
		if (decl->count && entry.count != decl->count) {
			job->log(job, stream->file->path, fmdlt_format,
				 "format(%s): IFD%u[%d].%d unexpected count %u",
				 stream->file->path,
				 (unsigned)ifd_index, i,
				 (unsigned)entry.count);
			continue;
		}
		hook(ctx, &entry, ifd_index);
	}
	uint32_t next_ifd = ctx->bits(p, 0, 32);
	if (fmd_tiff_trace)
		job->log(job, stream->file->path, fmdlt_trace,
			 "IFD%u: next IFD @ %u",
			 (unsigned)ifd_index, (unsigned)next_ifd);
	if (ifd_type == 0 && next_ifd)
		return fmdp_tiff_do_ifd(ctx, 0, ifd_index + 1, next_ifd, hook);
	return 0;		/* No more */
}


static int
fmdp_tiff_add_frac(struct FmdpTiffScanContext *ctx,
		   struct FmdpTiffIfdEntry *entry,
		   enum FmdElemType elemtype,
		   int as_rational)
{
	assert(ctx);
	assert(entry);
	assert(entry->type == fmdp_tet_rational);
	assert(entry->count == 1);

	/* References a pair of longs: numerator and denominator */
	struct FmdStream *stream = ctx->stream;
	const uint8_t *p = stream->get(stream, entry->v_long, 8);
	if (!p)
		return -1;
	if (as_rational) {
		int num = ctx->bits(p, 0, 32), denom = ctx->bits(p, 32, 32);
		return fmdp_add_rational(stream->file, elemtype, num, denom);
	} else {
		double num = ctx->bits(p, 0, 32), denom = ctx->bits(p, 32, 32);
		return fmdp_add_frac(stream->file, elemtype, num / denom);
	}
}


static int
fmdp_tiff_add_text(struct FmdpTiffScanContext *ctx,
		   struct FmdpTiffIfdEntry *entry,
		   enum FmdElemType elemtype)
{
	assert(ctx);
	assert(entry);
	assert(entry->type == fmdp_tet_ascii);
	assert(entry->count);

	struct FmdStream *stream = ctx->stream;
	if (entry->count <= 4)
		return fmdp_add_text(stream->file, elemtype,
				     entry->v_char, entry->count - 1);
	/* A referenced string */
	const uint8_t *p = stream->get(stream, entry->v_long, entry->count);
	if (!p)
		return -1;
	return fmdp_add_text(stream->file, elemtype,
			     (const char*)p, entry->count - 1);
}


static int
fmdp_tiff_add_bps(struct FmdpTiffScanContext *ctx)
{
	assert(ctx);

	struct FmdStream *stream = ctx->stream;
	struct FmdFile *file = stream->file;
	if (ctx->bits_per_sample.count != ctx->samples_per_pixel) {
		struct FmdScanJob *job = stream->job;
		job->log(job, file->path, fmdlt_format,
			 "format(%s): %d (bits/sample) != %d (s/pix)",
			 file->path, ctx->bits_per_sample.count,
			 ctx->samples_per_pixel);
		return (errno = EPROTONOSUPPORT), -1;
	}

	uint16_t i, v = 0, n = ctx->bits_per_sample.count;
	if (n <= 2) {		/* Inline */
		for (i = 0; i < n; ++i)
			v += ctx->bits_per_sample.v_short[i];
	} else {		/* Referenced */
		const uint8_t *p = stream->get(stream,
					       ctx->bits_per_sample.offs,
					       2 * n);
		if (!p)
			return -1;
		for (i = 0; i < n; ++i)
			v += ctx->bits(p, i * 16, 16);
	}
	return fmdp_add_n(file, fmdet_bits_per_sample, v);
}


int
fmdp_do_tiff(struct FmdStream *stream)
{
	assert(stream);
	if (!stream)
		return (errno = EINVAL), -1;

	struct FmdScanJob *job = stream->job;
	struct FmdFile *file = stream->file;

	const uint8_t *p = stream->get(stream, 0, 8);
	if (!p)
		return -1;

	struct FmdpTiffScanContext ctx;
	memset(&ctx, 0, sizeof ctx);
	ctx.stream = stream;
	ctx.samples_per_pixel = 1; /* default */
	if (p[0] == 'I' && p[1] == 'I') {
		ctx.bits = &fmdp_get_bits_le;
	} else if (p[0] == 'M' && p[1] == 'M') {
		ctx.bits = &fmdp_get_bits_be;
	} else {		/* Doesn't look like a TIFF-stream */
		return (errno = EPROTONOSUPPORT), 1;
	}

	const uint32_t ifd_offs = (uint32_t)ctx.bits(p, 32, 32);
	int res = fmdp_tiff_do_ifd(&ctx, /*type*/0, /*index*/0, ifd_offs,
				   &fmdp_tiff_do_baseline_ifd);
	if (res)
		return res;

	if (!ctx.width || !ctx.height) {
		job->log(job, file->path, fmdlt_format,
			 "format(%s): missing required fields",
			 file->path);
		return (errno = EPROTONOSUPPORT), 1;
	}

	stream->file->filetype = fmdft_raster;
	stream->file->mimetype = "image/tiff";

	if (res == 0 && ctx.exififd_offs)
		res = fmdp_tiff_do_ifd(&ctx, fmdp_ttt_exififd, 0,
				       ctx.exififd_offs,
				       &fmdp_tiff_do_exififd);
	if (res == 0 && ctx.gpsifd_offs)
		res = fmdp_tiff_do_ifd(&ctx, fmdp_ttt_gpsifd, 0,
				       ctx.gpsifd_offs,
				       &fmdp_tiff_do_gpsifd);

	if (res == 0)
		res = fmdp_add_n(file, fmdet_frame_width, ctx.width);
	if (res == 0)
		res = fmdp_add_n(file, fmdet_frame_height, ctx.height);
	if (res == 0 && ctx.samples_per_pixel)
		res = fmdp_add_n(file, fmdet_num_channels,
				 ctx.samples_per_pixel);
	if (res == 0 && ctx.bits_per_sample.tag)
		res = fmdp_tiff_add_bps(&ctx);

	if (res == 0 && ctx.docname.tag)
		res = fmdp_tiff_add_text(&ctx, &ctx.docname, fmdet_title);
	if (res == 0 && ctx.description.tag)
		res = fmdp_tiff_add_text(&ctx, &ctx.description,
					 fmdet_description);
	if (res == 0 && ctx.devicevendor.tag)
		res = fmdp_tiff_add_text(&ctx, &ctx.devicevendor,
					 fmdet_creator);
	if (res == 0 && ctx.devicemodel.tag)
		res = fmdp_tiff_add_text(&ctx, &ctx.devicemodel,
					 fmdet_creator);
	if (res == 0 && ctx.software.tag)
		/* XXX: fmdet_creator used for device vendor & model
		 * and software */
		res = fmdp_tiff_add_text(&ctx, &ctx.software, fmdet_creator);
	if (res == 0 && ctx.artist.tag)
		res = fmdp_tiff_add_text(&ctx, &ctx.artist, fmdet_artist);

	if (res == 0 && ctx.exposure_time.tag)
		res = fmdp_tiff_add_frac(&ctx, &ctx.exposure_time,
					 fmdet_exposure_time, /*rational*/1);
	if (res == 0 && ctx.fnumber.tag)
		res = fmdp_tiff_add_frac(&ctx, &ctx.fnumber,
					 fmdet_fnumber, /*rational*/0);
	if (res == 0 && ctx.iso_speed.tag && ctx.iso_speed.count == 1)
		/* Count defined as N in [2]? */
		res = fmdp_add_n(file, fmdet_iso_speed,
				 ctx.iso_speed.v_short[0]);
	if (res == 0 && ctx.focal_length.tag)
		res = fmdp_tiff_add_frac(&ctx, &ctx.focal_length,
					 fmdet_focal_length, /*rational*/0);
	if (res == 0 && ctx.focal_length35.tag)
		res = fmdp_add_frac(file, fmdet_focal_length35,
				    ctx.focal_length35.v_short[0]);

	return res;
}
