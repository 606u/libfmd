#if !defined (LIB_FILE_METADATA_H)
#  define LIB_FILE_METADATA_H

#  include <stdio.h>
#  include <sys/stat.h>

enum FmdScanFlags {
	/* read metadata for item at given |location| only, */
	fmdsf_single = 0,
	/* or scan recursively at this |location| */
	fmdsf_recursive = 1 << 0,

	/* also scan for metadata */
	fmdsf_metadata = 1 << 1,
	/* also scan archived files (not currently implemented) */
	fmdsf_archives = 1 << 2
};

enum FmdLogType {
	fmdlt_trace,  /* Generic debug message */
	fmdlt_format, /* Some sort of parse error or corrupted file */
	fmdlt_oserr,  /* OS-reported error, like I/O or perm */
	fmdlt_use     /* Error, related to libfmd usage */
};

/* XXX: Consult RFC5013: The Dublin Core Metadata Element Set */
enum FmdFileType {
	fmdft_file,
	fmdft_directory,	/* ? */
	fmdft_media,
	fmdft_audio,
	fmdft_video,
	fmdft_raster,
	fmdft_vector,
	fmdft_text,
	fmdft_richtext,
	fmdft_spreadsheet,
	fmdft_presentation,
	fmtft_mail,
};
extern const char *fmd_filetype[];

/* Notice: some fields, like artist, might occur more than once */
enum FmdElemType {
	fmdet_title,		/* text */
	fmdet_creator,		/* text */
	fmdet_subject,		/* text */
	fmdet_description,	/* text */
	fmdet_artist,		/* text */
	fmdet_performer,	/* text */
	fmdet_album,		/* text */
	fmdet_genre,		/* text */
	fmdet_trackno,		/* n */
	fmdet_date,		/* timestamp */
	fmdet_isrc,		/* text */
	fmdet_duration,		/* in seconds; frac */
	fmdet_sampling_rate,	/* n */
	fmdet_num_channels,	/* n */
	fmdet_bits_per_sample,	/* n */

	fmdet_other,		/* text: key=value */
};
extern const char *fmd_elemtype[];

enum FmdDataType {
	fmddt_n,
	fmddt_frac,
	fmddt_timestamp,
	fmddt_text,
};
extern const char *fmd_datatype[];

struct FmdElem {
	struct FmdElem *next;

	enum FmdElemType elemtype;
	enum FmdDataType datatype;

	union {
		long n;
		double frac;
		time_t timestamp;
		char text[1];
	};
};

struct FmdFile {
	struct FmdFile *next;
	enum FmdFileType filetype;
	const char *mimetype;
	struct FmdElem *metadata;
	struct stat stat;
	char *name, path[1];
};

struct FmdScanJob {
	const char *location;
	enum FmdScanFlags flags;

	/* fmd_scan() will fill this upon successful completion. Shall
	 * be freed with fmd_free() */
	struct FmdFile *first_file;

	/* Hooks: */
	/* For logging. fmd_scan() will assign a dummy hook if |log|
	 * is null to avoid a test before each invocation */
	void (*log)(struct FmdScanJob *job,
		    const char *path,
		    enum FmdLogType lt,
		    const char *fmt, ...);

	/* Called when starting to scan file. Will skip scanning
	 * |path| if hook is assigned and returns non-zero */
	int (*begin)(struct FmdScanJob *job, const char *path);
	/* Called when finished with scanning file. Will not add
	 * |file| to the chain if hook is assigned and returns
	 * non-zero */
	int (*finish)(struct FmdScanJob *job, struct FmdFile *file);

	/* Metrics/Statistics. n_ -> # of, v_ -> volume/octets */
	size_t n_filopens, n_diropens;
	size_t n_physreads, n_logreads;
	off_t v_physreads, v_logreads;
	size_t n_cachehits, n_cachemisses;
};

/* Read metadata from a file or a directory tree at |job->location|,
 * considering |job->flags| */
int fmd_scan(struct FmdScanJob *job);

void fmd_free(struct FmdFile *item);
void fmd_free_chain(struct FmdFile *head);

void fmd_print_elem(const struct FmdElem *elem,
		    FILE *where);
void fmd_print_file(const struct FmdFile *file,
		    int with_metadata,
		    FILE *where);

#endif /* LIB_FILE_METADATA_H defined? */
