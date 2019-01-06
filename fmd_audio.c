#include "fmd_priv.h"

#include <assert.h>
#include <limits.h>

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
	size_t len = FMDP_READ_PAGE_SZ;
	if ((off_t)len > stream->file->stat.st_size)
		len = (size_t)stream->file->stat.st_size;
	const uint8_t *p = stream->get(stream, 0, len);
	if (!p)
		return -1;

	/* Be lazy and expect all the metadata to fit in the first
	 * page */
	const uint8_t *endp = p + len;
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
