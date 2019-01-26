#include "fmd_priv.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

static struct FmdElem*
fmdp_add(struct FmdFile *file,
	 enum FmdElemType elemtype, enum FmdDataType datatype,
	 size_t extrasize)
{
	assert(file);
	/* XXX: align size? */
	size_t sz = sizeof (struct FmdElem) + extrasize;
	struct FmdElem *elem = (struct FmdElem*)calloc(1, sz);
	if (elem) {
		elem->elemtype = elemtype;
		elem->datatype = datatype;
		elem->next = file->metadata;
		file->metadata = elem;
		return elem;
	}
	return 0;
}

int
fmdp_add_n(struct FmdFile *file,
	   enum FmdElemType elemtype, long value)
{
	assert(file);
	struct FmdElem *elem = fmdp_add(file, elemtype, fmddt_n, 0);
	if (elem) {
		elem->n = value;
		return 0;
	}
	return -1;
}

int
fmdp_add_frac(struct FmdFile *file,
	      enum FmdElemType elemtype, double value)
{
	assert(file);
	struct FmdElem *elem = fmdp_add(file, elemtype, fmddt_frac, 0);
	if (elem) {
		elem->frac = value;
		return 0;
	}
	return -1;
}

int
fmdp_add_timestamp(struct FmdFile *file,
		   enum FmdElemType elemtype, time_t value)
{
	assert(file);
	struct FmdElem *elem = fmdp_add(file, elemtype, fmddt_timestamp, 0);
	if (elem) {
		elem->timestamp = value;
		return 0;
	}
	return -1;
}

int
fmdp_add_text(struct FmdFile *file,
	      enum FmdElemType elemtype, const char *s, int len)
{
	assert(file);
	assert(s);

	if (len == -1)
		len = strlen(s);

	struct FmdElem *elem = fmdp_add(file, elemtype, fmddt_text, len);
	if (elem) {
		memcpy(elem->text, s, len);
		elem->text[len] = '\0';
		return 0;
	}
	return -1;

}

int
fmdp_add_unicodewbom(struct FmdFile *file,
		     enum FmdElemType elemtype, const uint8_t *s, int len)
{
	assert(file);
	assert(s);
	assert(len > 0);
	assert((len % 2) == 0);

	if (len < 2 || (len % 2) != 0)
		return (errno = EINVAL), -1;

	/* Unicode BOM */
	const uint8_t *p, *endp = s + len;
	if (s[0] == 0xff && s[1] == 0xfe) { /* big-endian */
		/* Iterate over Big Endian Unicode string to calculate
		 * the # of bytes needed to convert to UTF-8 */
		size_t b = 0;
		for (p = s; p != endp; p += 2) {
			uint16_t c = ((uint16_t)p[1] << 8) + p[0];
			if (c <= 0x7f)
				++b;
			else if (c <= 0x7ff)
				b += 2;
			else
				b += 3;
		}

		/* Allocate field large enough to fit resulted UTF-8
		 * string and convert */
		struct FmdElem *elem = fmdp_add(file, elemtype, fmddt_text, b);
		if (elem) {
			uint8_t *o = (uint8_t*)elem->text;
			for (p = s; p != endp; p += 2) {
				uint16_t c = ((uint16_t)p[1] << 8) + p[0];
				if (c <= 0x7f)
					*o++ = (uint8_t)c;
				else if (c <= 0x7ff) {
					*o++ = 0xc0 | (uint8_t)(c >> 6);
					*o++ = 0x80 | (uint8_t)(c & 0x3f);
				} else {
					*o++ = 0xe0 | (uint8_t)(c >> 12);
					*o++ = 0x80 | (uint8_t)((c >> 6) & 0x3f);
					*o++ = 0x80 | (uint8_t)(c & 0x3f);
				}
			}
			*o = '\0';
			return 0;
		}

	} else if (s[0] == 0xfe && s[1] == 0xff) { /* little-endian */
		size_t b = 0;
		for (p = s; p != endp; p += 2) {
			uint16_t c = ((uint16_t)p[0] << 8) + p[1];
			if (c <= 0x7f)
				++b;
			else if (c <= 0x7ff)
				b += 2;
			else
				b += 3;
		}

		struct FmdElem *elem = fmdp_add(file, elemtype, fmddt_text, b);
		if (elem) {
			uint8_t *o = (uint8_t*)elem->text;
			for (p = s; p != endp; p += 2) {
				uint16_t c = ((uint16_t)p[0] << 8) + p[1];
				if (c <= 0x7f)
					*o++ = (uint8_t)c;
				else if (c <= 0x7ff) {
					*o++ = 0xc0 | (uint8_t)(c >> 6);
					*o++ = 0x80 | (uint8_t)(c & 0x3f);
				} else {
					*o++ = 0xe0 | (uint8_t)(c >> 12);
					*o++ = 0x80 | (uint8_t)((c >> 6) & 0x3f);
					*o++ = 0x80 | (uint8_t)(c & 0x3f);
				}
			}
			*o = '\0';
			return 0;
		}
	} else
		return (errno = EINVAL), -1;
	return -1;		/* out-of-memory */
}


long
fmdp_parse_decimal(const char *text, size_t len)
{
	const char *p = text, *endp = text + len;
	long rv = 0;
	while (p != endp) {
		if (isdigit((unsigned)*p))
			rv = rv * 10 + *p - '0';
		else {
			rv = LONG_MIN;
			break;
		}
		++p;
	}
	return rv;
}

int
fmdp_caseless_match(const char *text, size_t len,
		    const char *token)
{
	assert(text);
	assert(len);
	assert(token);

	const char *p = text, *endp = text + len;
	while (p != endp &&
	       tolower((unsigned)*p) == *token)
		++p, ++token;
	return p == endp && !*token;
}


int
fmdp_case_match(const char *text, size_t len,
		const char *token)
{
	assert(text);
	assert(len);
	assert(token);

	const char *p = text, *endp = text + len;
	while (p != endp && *p == *token)
		++p, ++token;
	return p == endp && !*token;
}


int
fmdp_match_token(const char *text, size_t len,
		 const struct FmdToken *tokens)
{
	assert(text);
	assert(len);
	assert(tokens);

	const struct FmdToken *it;
	for (it = tokens; it->name; ++it)
		if (fmdp_caseless_match(text, len, it->name))
			return it->value;
	return -1;
}


int
fmdp_match_token_exact(const char *text, size_t len,
		       const struct FmdToken *tokens)
{
	assert(text);
	assert(len);
	assert(tokens);

	const struct FmdToken *it;
	for (it = tokens; it->name; ++it) {
		if (fmdp_case_match(text, len, it->name))
			return it->value;
	}
	return -1;
}


struct FmdCachePage {
	uint8_t data[FMDP_READ_PAGE_SZ];
	off_t offs, len;

	/* Keeps track of number of cache hits, as well as generation
	 * (most/least recently used), so a newly read page, won't be
	 * reclaimed soon after being read */
	size_t hits, gen;
};
struct FmdCachedStream {
	struct FmdStream base;
	struct FmdStream *next;

	/* Keep several pages with file data to minimize I/O */
	/* Those pages are cache, as well as read buffers */
	struct FmdCachePage *last_hit;
	struct FmdCachePage page[FMDP_CACHE_PAGES];
	size_t gen;
};
#define FMDP_GET_CSTR(_stream)			\
	(struct FmdCachedStream*)((_stream) - offsetof(struct FmdCachedStream, base))

static const uint8_t*
fmdp_cached_stream_get(struct FmdStream *stream,
		       off_t offs, size_t len)
{
	assert(stream);
	assert(len);

	struct FmdCachedStream *cstr = FMDP_GET_CSTR(stream);
	/* Convert offsets, relative to end-of-file to absolute
	 * offsets; also make sure request is within file size */
	const off_t filesize = stream->file->stat.st_size;
	if (offs < 0)
		offs = filesize - offs;
	if (offs < 0 ||
	    offs + (off_t)len > filesize ||
	    !len) {
		errno = ERANGE;
		return 0;
	}

	/* Check if read request can be fulfilled from the cache */
	/* Also keep a track of best page to read into */
	struct FmdCachePage *unused = 0, *best = cstr->page;
	const struct FmdCachePage *endp = cstr->page + FMDP_CACHE_PAGES;
	struct FmdCachePage *it = cstr->last_hit;
	do {
		if (it->offs <= offs &&
		    it->offs + it->len >= offs + (off_t)len) {
			/* Found in cache */
			++it->hits;
			it->gen = ++cstr->gen;
			cstr->last_hit = it;

			/* Return from cache */
			return it->data + (offs - it->offs);
		}
		if (!it->len)
			unused = it;
		else if (it->gen < best->gen)
			best = it;
		if (++it == endp)
			it = cstr->page;
	} while (it != cstr->last_hit);

	/* Not found in cache, will read into |unused| or |best| */
	if (unused)
		best = unused;

	/* XXX: align read to optimal block size, if possible */
	/* XXX: align read not to include already cached page */
	const uint8_t *ptr =
		cstr->next->get(cstr->next, offs, FMDP_READ_PAGE_SZ);
	if (!ptr)
		return 0;

	memcpy(best->data, ptr, FMDP_READ_PAGE_SZ);
	best->offs = offs;
	best->len = FMDP_READ_PAGE_SZ;
	best->hits = 1;
	best->gen = ++cstr->gen;
	cstr->last_hit = best;

	assert(offs >= best->offs);
	return best->data + (offs - best->offs);
}


static void
fmdp_cached_stream_close(struct FmdStream *stream)
{
	assert(stream);

	struct FmdCachedStream *cstr = FMDP_GET_CSTR(stream);
	cstr->next->close(cstr->next);
	free(cstr);
}

struct FmdStream*
fmdp_cache_stream(struct FmdStream *stream)
{
	assert(stream);

	struct FmdCachedStream *cstr =
		(struct FmdCachedStream*)calloc(1, sizeof *cstr);
	if (cstr) {
		cstr->base.get = &fmdp_cached_stream_get;
		cstr->base.close = &fmdp_cached_stream_close;
		cstr->base.file = stream->file;
		cstr->next = stream;
		cstr->last_hit = cstr->page;
		stream = &cstr->base;
	}
	/* Cannot allocate memory -> return original |stream| */
	return stream;
}


struct FmdFileStream {
	struct FmdStream base;
	int fd;

	off_t offs;
	size_t len;
	uint8_t buf[FMDP_READ_PAGE_SZ];
};
#define FMDP_GET_FSTR(_stream)			\
	(struct FmdFileStream*)((_stream) - offsetof(struct FmdFileStream, base))

static const uint8_t*
fmdp_file_stream_get(struct FmdStream *stream,
		     off_t offs, size_t len)
{
	assert(stream);
	assert(len);

	struct FmdFileStream *fstr = FMDP_GET_FSTR(stream);

	if (fstr->offs <= offs &&
	    fstr->offs + fstr->len >= offs + len) {
		/* Return from cache */
		return fstr->buf + (offs - fstr->offs);
	}

	/* XXX: align if requested size is smaller than page size */
	off_t realoffs = lseek(fstr->fd, offs, SEEK_SET);
	if (realoffs == -1)
		return 0;
	if (realoffs != offs) {
		errno = ENOTRECOVERABLE;
		return 0;
	}

	ssize_t reallen = read(fstr->fd, fstr->buf, FMDP_READ_PAGE_SZ);
	if (reallen == -1)
		return 0;
	fstr->offs = realoffs;
	fstr->len = reallen;
	if (reallen < (ssize_t)len) {
		errno = ERANGE;
		return 0;
	}

	return fstr->buf + (offs - fstr->offs);
}

static void
fmdp_file_stream_close(struct FmdStream *stream)
{
	assert(stream);

	struct FmdFileStream *fstr = FMDP_GET_FSTR(stream);
	close(fstr->fd);
	free(fstr);
}

struct FmdStream*
fmdp_open_file(int dirfd, struct FmdFile *file, int cached)
{
	assert(file);

	struct FmdFileStream *fstr =
		(struct FmdFileStream*)calloc(1, sizeof *fstr);
	if (!fstr)
		return 0;

	fstr->base.get = &fmdp_file_stream_get;
	fstr->base.close = &fmdp_file_stream_close;
	fstr->base.file = file;
	fstr->fd = openat(dirfd, dirfd != AT_FDCWD ? file->name : file->path,
			  O_RDONLY);
	if (fstr->fd == -1) {
		free(fstr);
		return 0;
	}

	/* Issue a request to read file header */
	struct FmdStream *res = &fstr->base;
	if (cached) {
		struct FmdStream *c = fmdp_cache_stream(res);
		if (c)
			res = c;
	}
	(void)fmdp_file_stream_get(res, 0, FMDP_READ_PAGE_SZ);
	return res;
}


long
fmdp_get_bits_be(const uint8_t *p, size_t offs, size_t len)
{
	assert(p);
	assert(len);

	/* Position at initial byte */
	size_t o = offs / 8;
	p += o;
	offs -= o * 8;

	/* Extract the least-significant bits from the first byte */
	size_t have = 8 - (offs % 8);
	size_t extra = len > have ? 0 : have - len;
	size_t bits = len > have ? have : len;
	long rv = (*p >> extra) & (0xff >> (8 - bits));

	/* From here on |last| is always 7, |rem| is always 8, |offs|
	 * is no-longer used */
	++p;
	len -= bits;

	while (len) {
		/* Start from most-significant bits */
		extra = len > 8 ? 0 : 8 - len;
		bits = len > 8 ? 8 : len;
		rv <<= bits;
		rv |= (*p >> extra) & (0xff >> (8 - bits));
		++p;
		len -= bits;
	}
	return rv;
}


int
fmdp_probe_file(struct FmdScanJob *job,
		int dirfd,
		struct FmdFile *file)
{
	assert(job);
	assert(file);
	if (!job || !file)
		return (errno = EINVAL), -1;

	/* File should have minimum length in order to probe it */
	if (file->stat.st_size < 256)
		return 0;

	struct FmdStream *stream = fmdp_open_file(dirfd, file, /*cache*/1);
	if (!stream) {
		job->log(job, file->path, fmdlt_oserr, "%s(%s): %s",
			 "fmdp_open_file", file->path, strerror(errno));
		return -1;
	}
	stream->job = job;

	size_t len = FMDP_READ_PAGE_SZ;
	if ((off_t)len > stream->file->stat.st_size)
		len = (size_t)stream->file->stat.st_size;
	const uint8_t *p = stream->get(stream, 0, len);
	if (p) {
		/* Attempt to deduce file type from header magic */
		/* XXX: Replace with some sort of a table to be iterated */
		if (!memcmp(p, "fLaC", 4) &&
		    fmdp_do_flac(stream) == 0)
			goto end;
		if (p[0] == 'I' && p[1] == 'D' && p[2] == '3' &&
		    p[3] < 0xff && p[4] < 0xff &&
		    p[6] < 0x80 && p[7] < 0x80 && p[8] < 0x80 && p[9] < 0x80 &&
		    fmdp_do_mp3v2(stream) == 0)
			goto end;
	} else
		job->log(job, file->path, fmdlt_oserr, "%s(%s): %s",
			 "read", file->path, strerror(errno));

end:
	stream->close(stream);
	return 0;
}
