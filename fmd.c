#include "fmd.h"
#include "fmd_priv.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

const char *fmd_filetype[] = {
	"file",
	"directory",
	"media",
	"audio",
	"video",
	"raster",
	"vector",
	"text",
	"richtext",
	"spreadsheet",
	"presentation",
	"mail",
};
const char *fmd_elemtype[] = {
	"title",
	"creator",
	"subject",
	"description",
	"artist",
	"performer",
	"album",
	"genre",
	"trackno",
	"date",
	"isrc",
	"duration",
	"sampling_rate",
	"num_channels",
	"bits_per_sample",
	"frame_width",
	"frame_height",
	"exposure_time",
	"fnumber",
	"iso_speed",
	"focal_length",
	"focal_length35",

	"other",
};
const char *fmd_datatype[] = {
	"n",
	"frac",
	"timestamp",
	"rational",
	"text",
};

void
fmd_print_elem(const struct FmdElem *elem,
	       FILE *where)
{
	assert(elem);
	assert(where);
	if (!elem || !where)
		return;

	assert(elem->elemtype < sizeof(fmd_elemtype) / sizeof(fmd_elemtype[0]));
	const char *name = fmd_elemtype[elem->elemtype];
	struct tm tm;
	switch (elem->datatype) {
	case fmddt_n:
		fprintf(where, "\t%s: %ld\n", name, elem->n);
		return;
	case fmddt_frac:
		fprintf(where, "\t%s: %f\n", name, elem->frac);
		return;
	case fmddt_timestamp:
		if (localtime_r(&elem->timestamp, &tm))
			fprintf(where, "\t%s: %04d-%02d-%02d %02d:%02d:%02d\n",
				name,
				tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
				tm.tm_hour, tm.tm_min, tm.tm_sec);
		else
			fprintf(where, "\t%s: (timestamp)\n", name);
		return;
	case fmddt_rational:
		fprintf(where, "\t%s: %d/%d\n", name,
			elem->numerator, elem->denominator);
		return;
	case fmddt_text:
		if (elem->elemtype == fmdet_other) {
			/* text is "key=value" */
			const char *eq = strchr(elem->text, '=');
			assert(eq);
			if (eq) {
				fprintf(where, "\t%.*s: '%s'\n",
					(int)(eq - elem->text), elem->text,
					eq + 1);
				return;
			}
		}
		fprintf(where, "\t%s: '%s'\n", name, elem->text);
		return;
	}
	assert(0);		/* bad datatype */
}


void
fmd_print_file(const struct FmdFile *file,
	       int with_metadata,
	       FILE *where)
{
	assert(file);
	assert(where);
	if (!file || !where)
		return;

	fprintf(where, "%s (%s)\n", file->path, file->name);
	assert(file->filetype < sizeof(fmd_filetype) / sizeof(fmd_filetype[0]));
	fprintf(where, "  filetype: '%s'\n", fmd_filetype[file->filetype]);
	fprintf(where, "  mimetype: '%s'\n", file->mimetype);
	fprintf(where, "  dev %ld, ino %ld, links %ld\n",
		(long)file->stat.st_dev, (long)file->stat.st_ino,
		(long)file->stat.st_nlink);
	fprintf(where, "  size %ld, blksize %ld, blocks %ld\n",
		(long)file->stat.st_size, (long)file->stat.st_blksize,
		(long)file->stat.st_blocks);
	struct tm tm;
	if (localtime_r(&file->stat.st_atime, &tm))
		fprintf(where, "  atime: %04d-%02d-%02d %02d:%02d:%02d\n",
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec);
	if (localtime_r(&file->stat.st_mtime, &tm))
		fprintf(where, "  mtime: %04d-%02d-%02d %02d:%02d:%02d\n",
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec);
	if (localtime_r(&file->stat.st_ctime, &tm))
		fprintf(where, "  ctime: %04d-%02d-%02d %02d:%02d:%02d\n",
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec);
	fprintf(where, "  uid %d, gid %d, mode 0%o\n",
		(int)file->stat.st_uid, (int)file->stat.st_gid,
		(int)file->stat.st_mode);
	if (with_metadata) {
		struct FmdElem *it = file->metadata;
		for (; it; it = it->next)
			fmd_print_elem(it, where);
	}
}


static int
fmd_scan_file(struct FmdScanJob *job,
	      int dirfd,
	      const char *path,
	      struct FmdFile **info)
{
	assert(job);
	assert(path);
	assert(info);
	if (!job || !path || !info)
		return (errno = EINVAL), -1;

	size_t path_len = strlen(path);
	size_t sz = sizeof(struct FmdFile) + path_len;
	struct FmdFile *file = (struct FmdFile*)calloc(1, sz);
	if (!file) {
		job->log(job, path, fmdlt_oserr, "%s(%u): %s",
			 "calloc", (unsigned)sz, strerror(ENOMEM));
		FMDP_X(-1);
		return -1;	/* errno should be ENOMEM */
	}

	strcpy(file->path, path);
	file->name = strrchr(file->path, '/');
	file->name = file->name ? file->name + 1 : file->path;

	int stflags = 0;	/* AT_SYMLINK_NOFOLLOW? */
	int res = fstatat(dirfd, dirfd != AT_FDCWD ? file->name : file->path,
			  &file->stat, stflags);
	if (res != 0) {
		job->log(job, path, fmdlt_oserr, "%s(%s): %s",
			 "fstatat", path, strerror(errno));
		free(file);
		FMDP_X(res);
		return res;
	}

	const int is_dir = S_ISDIR(file->stat.st_mode);
	if (!is_dir)
		file->mimetype = "application/binary-stream";
	else
		file->filetype = fmdft_directory;

	*info = file;
	if (!is_dir && (job->flags & fmdsf_metadata) == fmdsf_metadata)
		return fmdp_probe_file(job, dirfd, file), 0;
	return 0;
}


static int
fmd_scan_hier(struct FmdScanJob *job,
	      int parent_dirfd,
	      const char *path,
	      struct FmdFile **info)
{
	assert(job);
	assert(path);
	assert(info);
	if (!job || !path || !info)
		return (errno = EINVAL), -1;

	/* XXX: add up statistics to keep track of errors */

	enum { fullpath_sz = 2048 };
	const size_t path_len = strlen(path) + 1;
	if (path_len + 10 > fullpath_sz) {
		errno = ENAMETOOLONG;
		job->log(job, path, fmdlt_use, "%s(%s): %s",
			 "path", path, strerror(errno));
		FMDP_X(-1);
		return -1;
	}

	int dirfd = openat(parent_dirfd, path, O_RDONLY | O_DIRECTORY);
	if (dirfd == -1) {
		job->log(job, path, fmdlt_oserr, "%s(%s): %s",
			 "openat", path, strerror(errno));
		FMDP_X(-1);
		return -1;
	}
	++job->n_filopens;
	DIR *dirp = fdopendir(dirfd);
	if (!dirp) {
		job->log(job, path, fmdlt_oserr, "%s(%s): %s",
			 "fdopendir", path, strerror(errno));
		close(dirfd);	/* could lose errno */
		FMDP_X(-1);
		return -1;
	}
	++job->n_diropens;

	struct FmdFile *rv = 0;
	{
		struct FmdFile *tail = 0;
		char fullpath[fullpath_sz];
		strcpy(fullpath, path);
		fullpath[path_len - 1] = '/';

		struct dirent *entry;
		while ((entry = readdir(dirp)) != NULL) {
			size_t len = strlen(entry->d_name);
			if (path_len + len + 1 >= sizeof fullpath)
				continue; /* path too long */
			if (entry->d_name[0] == '.' &&
			    (entry->d_name[1] == '\0' ||
			     (entry->d_name[1] == '.' &&
			      entry->d_name[2] == '\0')))
				continue; /* omit . and .. */

			struct FmdFile *file = 0;
			strcpy(fullpath + path_len, entry->d_name);
			int res = fmd_scan_file(job, dirfd, fullpath, &file);
			if (!res && file) {
				if (tail)
					tail->next = file;
				else
					rv = file;
				tail = file;
			}
			/* XXX: else keep track of errors */
		}
	}

	/* Breath first; time to scan directories */
	struct FmdFile *it = rv;
	while (it) {
		if (it->filetype == fmdft_directory &&
		    it->name[0] != '.') {
			struct FmdFile *children = 0;
			int res = fmd_scan_hier(job, dirfd, it->name,
						&children);
			if (!res && children) {
				/* Insert children right after
				 * directory they're in. Then skip
				 * through all children, because we
				 * should add prev next entry last */
				struct FmdFile *next = it->next;
				it->next = children;
				while (it->next)
					it = it->next;
				it->next = next;
			}
		}
		it = it->next;
	}
	/* closedir(3) will take care to close |dirfd| */
	closedir(dirp);

	*info = rv;
	return 0;
}


static void
fmd_dummy_log(struct FmdScanJob *job,
	      const char *path,
	      enum FmdLogType lt,
	      const char *fmt, ...)
{
	assert(job); (void)job;
	assert(path); (void)path;
	(void)lt;
	assert(fmt); (void)fmt;
}


int
fmd_scan(struct FmdScanJob *job)
{
	assert(job);
	assert(job->location);
	if (!job || !job->location)
		return (errno = EINVAL), -1;

	if (!job->log)
		/* Ensure |job->log| is always available. That would
		 * make code down further simpler, as a test, whether
		 * |log| is assigned, can be avoided everywhere */
		job->log = &fmd_dummy_log;

	if ((job->flags & fmdsf_recursive) != fmdsf_recursive)
		return fmd_scan_file(job, AT_FDCWD, job->location,
				     &job->first_file);
	return fmd_scan_hier(job, AT_FDCWD, job->location, &job->first_file);
}


void
fmd_free(struct FmdFile *item)
{
	if (!item)
		return;
	struct FmdElem *it = item->metadata;
	while (it) {
		struct FmdElem *next = it->next;
		free(it);
		it = next;
	}
	free(item);
}


void
fmd_free_chain(struct FmdFile *head)
{
	struct FmdFile *it = head;
	while (it) {
		struct FmdFile *next = it->next;
		fmd_free(it);
		it = next;
	}
}
