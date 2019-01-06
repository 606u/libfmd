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

struct FmdStream {
	/* Reads |len| bytes from |stream|, starting at |offs|, and
	 * returns a pointer to a readen data; returns 0 if request
	 * cannot be fulfilled, and sets |errno|. Signals ERANGE on
	 * attempt to read before start or after end of file.
	 *
	 * Negative |offs| is translated to a seek, relative from
	 * end-of-file.
	 *
	 * |len| should be up to FMDP_READ_PAGE_SZ.
	 *
	 * The pointer and the data referred are valid until next
	 * |get()| or |close()| method calls */
	const uint8_t* (*get)(struct FmdStream *stream,
			      off_t offs, size_t len);

	/* Closes given |stream| */
	void (*close)(struct FmdStream *stream);

	struct FmdFile *file;
};
struct FmdStream* fmdp_open_file(int dirfd, struct FmdFile *file, int cached);

/* Returns |len| big-endian bits from |offs|, also in bits */
long fmdp_get_bits_be(const uint8_t *p, size_t offs, size_t len);

int fmdp_probe_file(int dirfd, struct FmdFile *info);

int fmdp_do_flac(struct FmdStream *stream);

#endif /* LIB_FILE_METADATA_PRIV_H defined? */
