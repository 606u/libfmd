#include "fmd_priv.h"

#include <assert.h>
#include <ctype.h>
#include <limits.h>
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
fmdp_match_token(const char *text, size_t len,
		 const struct FmdToken *tokens)
{
	const struct FmdToken *it;
	for (it = tokens; it->name; ++it)
		if (fmdp_caseless_match(text, len, it->name))
			return it->value;
	return -1;
}


struct FmdReadState*
fmdp_open(int dirfd, struct FmdFile *file)
{
	assert(file);

	struct FmdReadState *res =
		(struct FmdReadState*)calloc(1, sizeof *res);
	if (!res)
		return 0;

	res->file = file;
	res->dirfd = dirfd;
	res->fd = openat(dirfd, dirfd != AT_FDCWD ? file->name : file->path,
			 O_RDONLY);
	if (res->fd == -1) {
		free(res);
		return 0;
	}
	res->last_hit = res->page;

	/* Issue a request to read file header */
	struct FmdBlock b = { 0, 0, FMDP_READ_PAGE_SZ };
	fmdp_read(res, &b);

	return res;
}


void
fmdp_close(struct FmdReadState *rst)
{
	assert(rst);
	close(rst->fd);
	free(rst);
}


int
fmdp_read(struct FmdReadState *rst,
	  struct FmdBlock *b)
{
	assert(rst);
	assert(b);

	/* Convert offsets, relative to end-of-file to absolute
	 * offsets; also make sure request is within file size */
	const off_t filesize = rst->file->stat.st_size;
	if (b->offs < 0)
		b->offs = filesize - b->offs;
	if (b->offs < 0)
		b->offs = 0;
	if (b->offs + b->len > filesize)
		b->len = filesize - b->offs;
	if (!b->len)
		return 0;	/* zero-length file, perhaps */

	/* Check if read request can be fulfilled from the cache */
	/* Also keep a track of best page to read into */
	struct FmdCachePage *unused = 0, *best = rst->page;
	const struct FmdCachePage *endp = rst->page + FMDP_CACHE_PAGES;
	struct FmdCachePage *it = rst->last_hit;
	do {
		if (it->offs <= b->offs &&
		    it->offs + it->len >= b->offs + b->len) {
			/* Found in cache */
			++it->hits;
			it->gen = ++rst->gen;
			rst->last_hit = it;

			/* Return from cache */
			b->ptr = it->data + (b->offs - it->offs);
			return 1;
		}
		if (!it->len)
			unused = it;
		else if (it->gen < best->gen)
			best = it;
		if (++it == endp)
			it = rst->page;
	} while (it != rst->last_hit);

	/* Not found in cache, will read into |unused| or |best| */
	if (unused)
		best = unused;

	/* XXX: align read to optimal block size, if possible */
	/* XXX: align read not to include already cached page */
	off_t offs = lseek(rst->fd, b->offs, SEEK_SET);
	if (offs == -1)
		return -1;

	ssize_t len = read(rst->fd, best->data, FMDP_READ_PAGE_SZ);
	if (len == -1)
		return -1;
	best->offs = offs;
	best->len = len;
	best->hits = 1;
	best->gen = ++rst->gen;
	rst->last_hit = best;

	assert(b->offs >= best->offs);
	b->ptr = best->data + (b->offs - best->offs);
	if (best->len < b->len)
		b->len = best->len;
	return b->len > 0;
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
fmdp_probe_file(int dirfd, struct FmdFile *info)
{
	assert(info);

	struct FmdReadState *rst = fmdp_open(dirfd, info);
	if (!rst)
		return -1;

	/* Attempt to deduce file type from header magic */
	/* XXX: Replace with some sort of a table to be iterated */
	struct FmdBlock hdr = { 0, 0, FMDP_READ_PAGE_SZ };
	int res = fmdp_read(rst, &hdr);
	if (res == 1) {
		assert(hdr.ptr);
		if (hdr.len > 4 &&
		    !memcmp(hdr.ptr, "fLaC", 4) &&
		    fmdp_do_flac(rst) == 0)
			goto end;
	}

end:
	fmdp_close(rst);
	return 0;
}
