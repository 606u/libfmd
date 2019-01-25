#include "fmd_priv.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define FMDP_ID3V234_FRHDR_SZ 10 /* ID3v2.[34] frame header size */

static int
fmdp_do_flac_stream_info(struct FmdStream *stream,
			 const uint8_t *si)
{
	assert(stream);
	assert(si);

	/* Stream info follows strict layout */
	long sample_rate = fmdp_get_bits_be(si, 80, 20);
	long channels = fmdp_get_bits_be(si, 80 + 20, 3) + 1;
	long bits_per_sample = fmdp_get_bits_be(si, 80 + 20 + 3, 5) + 1;
	/* XXX: 36 bits could overflow 32-bit long */
	long total_samples = fmdp_get_bits_be(si, 80 + 20 + 3 + 5, 36);

	struct FmdFile *file = stream->file;
	int res = fmdp_add_n(file, fmdet_sampling_rate, sample_rate);
	if (!res)
		res = fmdp_add_n(file, fmdet_num_channels, channels);
	if (!res)
		res = fmdp_add_n(file, fmdet_bits_per_sample, bits_per_sample);
	if (!res) {
		double duration = (double)total_samples / (double)sample_rate;
		res = fmdp_add_frac(file, fmdet_duration, duration);
	}
	return res;
}


/* Handles single Ogg Vorbis metadata field */
static int
fmdp_do_vorbis_md_field(struct FmdFile *file,
			const char *name, size_t name_len,
			const char *value, size_t value_len)
{
	assert(file);
	assert(name);
	assert(name_len);
	assert(value);

	static const struct FmdToken vorbis_fields[] = {
		{ "title", fmdet_title },
		{ "album", fmdet_album },
		{ "tracknumber", fmdet_trackno },
		{ "artist", fmdet_artist },
		{ "performer", fmdet_performer },
		{ "description", fmdet_description },
		{ "genre", fmdet_genre },
		{ "date", fmdet_date },
		{ "isrc", fmdet_isrc },
		{ 0, 0 }
	};
	int t = fmdp_match_token(name, name_len, vorbis_fields);
	if (t == -1)
		return 0;	/* no match found */
	if (t == fmdet_trackno) {
		long n = fmdp_parse_decimal(value, value_len);
		if (n != LONG_MIN)
			return fmdp_add_n(file, t, n);
		return 0;
	} else {
		return fmdp_add_text(file, t, value, value_len);
	}
}


/* Handles Ogg Vorbis comments section */
static int
fmdp_do_vorbis_comments(struct FmdStream *stream,
			const uint8_t *comment, size_t len)
{
	assert(stream);
	assert(comment);
	assert(len);

	if (len < 8)
		return 0;	/* two lengths minimum */

	/* Spec: https://xiph.org/vorbis/doc/v-comment.html */
	/* All lengths are 32-bit little-endian; text is in UTF-8 */
	struct FmdFile *file = stream->file;
	const uint8_t *p = comment, *endp = p + len;
#define FMDP_GET_LE32(_p)						\
	((_p)[0] | ((size_t)(_p)[1] << 8) | ((size_t)(_p)[2] << 16) |	\
	 ((size_t)(_p)[3] << 24))
	size_t n = FMDP_GET_LE32 (p); p += 4;
	if (p + n > endp)
		return 0;
	int res = fmdp_add_text(file, fmdet_creator, (const char*)p, n);
	if (res)
		return res;
	p += n;
	n = FMDP_GET_LE32 (p); p += 4;
	size_t i;
	for (i = 0; i < n && p + 4 <= endp; ++i) {
		size_t l = FMDP_GET_LE32 (p); p += 4;
		if (p + l > endp)
			break;
		const uint8_t *eq = p;
		while (eq < endp && *eq != '=')
			++eq;
		if (eq != endp) {
			res = fmdp_do_vorbis_md_field(file,
						      (const char*)p,
						      eq - p,
						      (const char*)eq + 1,
						      l - (eq - p) - 1);
			if (res)
				return res;
		}
		p += l;
	}
	return res;
}


int
fmdp_do_flac(struct FmdStream *stream)
{
	assert(stream);

	/* Format spec: https://xiph.org/flac/format.html#stream */
	FMDP_READ1STPAGE(stream, -1);
	p += 4;			/* fLaC */
	/* Iterate over metadata blocks */
	int last = 0;
	while (p + 4 <= endp && !last) {
		last = (*p & 0x80) != 0;
		int block_type = *p & 0x7f;
		int block_len = ((int)p[1] << 16) | ((int)p[2] << 8) | p[3];
		if (p + block_len <= endp) {
			const uint8_t *payload = p + 4;
			if (block_type == 0 &&
			    block_len == 34)
				/* stream info */
				fmdp_do_flac_stream_info(stream, payload);
			else if (block_type == 4 &&
				 block_len >= 8)
				/* vorbis comment */
				fmdp_do_vorbis_comments(stream, payload,
							block_len);
		}
		p += 4 + block_len;
	}

	stream->file->filetype = fmdft_audio;
	stream->file->mimetype = "audio/flac";

	return 0;
}


struct FmdID3v2FrameIterator {
	struct FmdFrameIterator base;

	struct FmdStream *stream;
	/* Current ID3v2 frame offset, initially 0. Incremented with
	 * |frame_size| when ..._next() is called */
	off_t offs;
	/* Offset of last ID3v2 header byte */
	off_t endoffs;

	/* A copy of current frame's frame_id; |frame.type| also
	 * points here and |frame.typelen| is set in ..._create() */
	uint8_t frame_id[4 + 1];
	/* Current ID3v2 frame size, including header; used as an
	 * offset to position to the next metadata frame */
	size_t frame_size;
};
#define GET_ID3V2(_iter)			\
	(struct FmdID3v2FrameIterator*)((char*)(_iter) - offsetof (struct FmdID3v2FrameIterator, base))

static int
fmdp_id3v234frit_next(struct FmdFrameIterator *iter)
{
	/* Frame iterator for ID3v2.3 and ID3v2.4 */
	assert(iter);
	if (!iter)
		return (errno = EINVAL), -1;

	struct FmdID3v2FrameIterator *id3it = GET_ID3V2(iter);
	id3it->offs += id3it->frame_size;
	if (id3it->offs + FMDP_ID3V234_FRHDR_SZ > id3it->endoffs)
		return 0;

	/* Frame header: 4-byte frame-id, 4-byte size, 2-byte flags */
	const uint8_t *p = id3it->stream->get(id3it->stream, id3it->offs,
					      FMDP_ID3V234_FRHDR_SZ);
	if (!p)
		return -1;

	memcpy(id3it->frame_id, p, 4);
	id3it->base.datalen = fmdp_get_bits_be(p, 4 * 8, 32);
	id3it->frame_size = FMDP_ID3V234_FRHDR_SZ + id3it->base.datalen;
	return 1;
}

static int
fmdp_id3v234frit_read(struct FmdFrameIterator *iter)
{
	assert(iter);
	if (!iter)
		return (errno = EINVAL), -1;

	struct FmdID3v2FrameIterator *id3it = GET_ID3V2(iter);
	const off_t offs = id3it->offs + FMDP_ID3V234_FRHDR_SZ;
	const size_t len = id3it->base.datalen;
	if (offs + (off_t)len > id3it->endoffs)
		return 0;

	const uint8_t *p = id3it->stream->get(id3it->stream, offs, len);
	if (p) {
		id3it->base.data = p;
		return 0;
	}
	return -1;
}

static void
fmdp_id3frit_free(struct FmdFrameIterator *iter)
{
	assert(iter);
	if (!iter)
		return;

	struct FmdID3v2FrameIterator *id3it = GET_ID3V2(iter);
	free(id3it);
}

static struct FmdFrameIterator*
fmdp_id3frit_create(struct FmdStream *stream)
{
	assert(stream);
	if (!stream)
		return (errno = EINVAL), (void*)0;

	FMDP_READ1STPAGE(stream, 0);
	const uint8_t id3ver = p[3];
	if (id3ver == 2)
		/* ID3v2.2 not supported at this time */
		return (errno = EPROTONOSUPPORT), (void*)0;

	struct FmdID3v2FrameIterator *id3it =
		(struct FmdID3v2FrameIterator*)calloc(1, sizeof *id3it);
	if (!id3it)
		return 0;

	id3it->base.next = &fmdp_id3v234frit_next;
	id3it->base.read = &fmdp_id3v234frit_read;
	/* XXX: _part() */
	id3it->base.free = &fmdp_id3frit_free;

	id3it->stream = stream;

	/* Those two are const: frame id is always 4 bytes; bytes in
	 * |id3it->frame_id| are changed from ..._next() */
	id3it->base.type = id3it->frame_id;
	id3it->base.typelen = 4;

	id3it->offs = 0;
	id3it->frame_size = 10; /* len of ID3v2 tag header */
	id3it->endoffs = ((((off_t)p[6] & 0x7f) << 21) |
			  (((off_t)p[7] & 0x7f) << 14) |
			  (((off_t)p[8] & 0x7f) << 7) |
			  ((off_t)p[9] & 0x7f));
	id3it->endoffs += id3it->frame_size;
	return &id3it->base;
}


static int
fmdp_do_id3_md_field(struct FmdFile *file,
		     struct FmdFrameIterator *iter)
{
	assert(file);
	assert(iter);

	static const struct FmdToken id3_fields[] = {
		{ "TIT2", fmdet_title },
		{ "TALB", fmdet_album },
		{ "TRCK", fmdet_trackno },
		{ "TOPE", fmdet_artist },
		{ "TPE1", fmdet_performer },
	/* XXX: COMM -> fmdet_description requires special handling */
		{ "TENC", fmdet_creator },
		{ "TDAT", fmdet_date },
		{ "TYER", fmdet_date },
		{ "TSRC", fmdet_isrc },
		{ 0, 0 }
	};
	int t = fmdp_match_token_exact((const char*)iter->type,
				       iter->typelen, id3_fields);
	if (t == -1)
		return 0;	/* no match found */
	if (iter->read(iter) == -1)
		return -1;	/* Can't read frame data */
	assert(iter->data);
	const char *value = (const char*)iter->data;
	const size_t value_len = iter->datalen;
	if (t == fmdet_trackno) {
		long n = fmdp_parse_decimal(value, value_len);
		if (n != LONG_MIN)
			return fmdp_add_n(file, t, n);
		return 0;
	} else {
		const uint8_t encoding = value[0];
		if (encoding == 0)
			/* ISO-8859-1 */
			return fmdp_add_text(file, t, value + 1,
					     value_len - 1);
		else if (encoding == 1)
			/* Unicode */
			return fmdp_add_unicodewbom(file, t,
						    (uint8_t*)value + 1,
						    value_len - 1);
		return 0;
	}
}


int
fmdp_do_mp3v2(struct FmdStream *stream)
{
	assert(stream);

	/* Format spec: http://id3.org/Developer%20Information */
	/* (Now obsolete) ID3v2 and ID3v2.3/2.4 are quite different */
	struct FmdFrameIterator *iter = fmdp_id3frit_create(stream);
	if (!iter)
		return -1;

	while (iter->next(iter)) {
		fmdp_do_id3_md_field(stream->file, iter);
	}

	iter->free(iter);

	stream->file->filetype = fmdft_audio;
	stream->file->mimetype = "audio/mpeg";

	return 0;
}
