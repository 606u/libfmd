#if !defined (LIB_FILE_METADATA_PRIV_H)
#  define LIB_FILE_METADATA_PRIV_H

#  include "fmd.h"
#  include <stdint.h>

#  if !defined (FMDP_READ_PAGE_SZ)
#    define FMDP_READ_PAGE_SZ 32768
#  endif
#  if !defined (FMDP_CACHE_PAGES)
#    define FMDP_CACHE_PAGES 4
#  endif

/* Adds metadata to |file| */
int fmdp_add_n(struct FmdFile *file,
	       enum FmdElemType elemtype, long value);
int fmdp_add_frac(struct FmdFile *file,
		  enum FmdElemType elemtype, double value);
int fmdp_add_timestamp(struct FmdFile *file,
		       enum FmdElemType elemtype, time_t value);
int fmdp_add_text(struct FmdFile *file,
		  enum FmdElemType elemtype, const char *s, int len);

/* Parses text to a decimal; returns LONG_MIN on error */
long fmdp_parse_decimal(const char *text, size_t len);

/* Returns non-zero if |text| matches to |token| (which should be
 * given lowercase) */
int fmdp_caseless_match(const char *text, size_t len,
			const char *token);

struct FmdToken {
	const char *name;
	int value;
};
/* Returns |value| for entry, whose |name| is matching to |text| or
 * -1; trailing token should be { 0, 0 } */
int fmdp_match_token(const char *text, size_t len,
		     const struct FmdToken *tokens);

struct FmdCachePage {
	uint8_t data[FMDP_READ_PAGE_SZ];
	off_t offs, len;

	/* Keeps track of number of cache hits, as well as generation
	 * (most/least recently used), so a newly read page, won't be
	 * reclaimed soon after being read */
	size_t hits, gen;
};

struct FmdReadState {
	struct FmdFile *file;
	int dirfd, fd;

	/* Keep several pages with file data to minimize I/O */
	/* Those pages are cache, as well as read buffers */
	struct FmdCachePage *last_hit;
	struct FmdCachePage page[FMDP_CACHE_PAGES];
	size_t gen;
};
struct FmdReadState* fmdp_open(int dirfd, struct FmdFile *file);
void fmdp_close(struct FmdReadState *rst);

/* Reads file block at given offset and length; returns 1 upon
 * success, 0 if not possible, -1 on failure */
struct FmdBlock {
	uint8_t *ptr;
	off_t offs, len;
};
int fmdp_read(struct FmdReadState *rst,
	      struct FmdBlock *b);

/* Returns |len| big-endian bits from |offs|, also in bits */
long fmdp_get_bits_be(const uint8_t *p, size_t offs, size_t len);

int fmdp_probe_file(int dirfd, struct FmdFile *info);

int fmdp_do_flac(struct FmdReadState *rst);

#endif /* LIB_FILE_METADATA_PRIV_H defined? */
