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

/* FMDP_X(res) macro is used to trace functions' failures */
#  if defined (_DEBUG)
#    define FMDP_X(_res)					\
	fprintf(stderr, "libfmd: %s (%s:%u) -> %d\n",		\
		__FUNCTION__, __FILE__, __LINE__, (_res))
#  else
#    define FMDP_X(_res)
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
/* Adds text with Unicode BOM (byte-order mark) */
int fmdp_add_unicodewbom(struct FmdFile *file,
			 enum FmdElemType elemtype, const uint8_t *s, int len);

/* Parses text to a decimal; returns LONG_MIN on error */
long fmdp_parse_decimal(const char *text, size_t len);

/* Returns non-zero if |text| matches to |token| (which should be
 * given lowercase) */
int fmdp_caseless_match(const char *text, size_t len,
			const char *token);
/* Same as |fmdp_caseless_match()| except comparison is exact */
int fmdp_case_match(const char *text, size_t len,
		    const char *token);

struct FmdToken {
	const char *name;
	int value;
};
/* Returns |value| for entry, whose |name| is matching to |text| or
 * -1; trailing token should be { 0, 0 } */
int fmdp_match_token(const char *text, size_t len,
		     const struct FmdToken *tokens);
/* Same as |fmdp_match_token()| except comparison is not case-less */
int fmdp_match_token_exact(const char *text, size_t len,
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

	struct FmdScanJob *job;
	struct FmdFile *file;
};
struct FmdStream* fmdp_open_file(int dirfd, struct FmdFile *file, int cached);

/* Returns |len| big-endian bits from |offs|, also in bits */
long fmdp_get_bits_be(const uint8_t *p, size_t offs, size_t len);

int fmdp_probe_file(struct FmdScanJob *job, int dirfd, struct FmdFile *info);

/* Reads 1st page or whole file, whatever is less, defines |len|, |p|
 * and |endp|; returns -1 on failure to do so */
#  define FMDP_READ1STPAGE(_stream, _failrv)			\
	size_t len = FMDP_READ_PAGE_SZ;				\
	if ((off_t)len > stream->file->stat.st_size)		\
		len = (size_t)stream->file->stat.st_size;	\
	const uint8_t *p = stream->get(stream, 0, len);		\
	if (!p)							\
		return (_failrv);				\
	const uint8_t *endp = p + len;				\
	(void)endp

/* Interface to iterate over stream frames (i.e. over ID3v2 or OGG
 * frames) */
struct FmdFrameIterator {
	/* Positions at 1st/next frame and fills |type/len| and
	 * |datalen|; returns 1 on success, 0 if no next frame, or -1
	 * on error */
	int (*next)(struct FmdFrameIterator *iter);

	/* Reads whole current frame, fills |data/len|. Frame size
	 * should not be larger than FMDP_READ_PAGE_SZ */
	int (*read)(struct FmdFrameIterator *iter);

	/* Returns pointer to |len| bytes starting at |offs| from
	 * current frame's data; partial read; invalidates |data| as
	 * well as pointers returned by past get() calls */
	const uint8_t* (*get)(struct FmdFrameIterator *iter,
			      off_t offs, size_t len);
	void (*free)(struct FmdFrameIterator *iter);

	struct FmdStream *stream;

	size_t typelen, datalen;
	const uint8_t *type;	/* 0, unless next() called */
	const uint8_t *data;	/* 0, unless read() called */
};

int fmdp_do_flac(struct FmdStream *stream);
int fmdp_do_mp3v2(struct FmdStream *stream);
int fmdp_do_bmff(struct FmdStream *stream);

#endif /* LIB_FILE_METADATA_PRIV_H defined? */
