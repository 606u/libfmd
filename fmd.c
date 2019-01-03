#include "fmd.h"
#include "fmd_priv.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

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
};
const char *fmd_datatype[] = {
	"n",
	"frac",
	"timestamp",
	"text",
};

void
fmd_print_elem(const struct FmdElem *elem,
	       FILE *where)
{
	assert(elem);
	assert(where);

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
	case fmddt_text:
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
fmd_scan_file(int dirfd,
	      const char *path,
	      enum FmdScanFlags flags,
	      struct FmdFile **info)
{
	assert(path);
	assert(info);

	size_t path_len = strlen(path);
	size_t sz = sizeof(struct FmdFile) + path_len;
	struct FmdFile *file = (struct FmdFile*)calloc(1, sz);
	if (!file)
		return -1;	/* errno should be ENOMEM */

	strcpy(file->path, path);
	file->name = strrchr(file->path, '/');
	file->name = file->name ? file->name + 1 : file->path;

	int stflags = 0;	/* AT_SYMLINK_NOFOLLOW? */
	int res = fstatat(dirfd, dirfd != AT_FDCWD ? file->name : file->path,
			  &file->stat, stflags);
	if (res != 0) {
		free(file);
		return res;
	}

	const int is_dir = S_ISDIR(file->stat.st_mode);
	if (!is_dir)
		file->mimetype = "application/binary-stream";
	else
		file->filetype = fmdft_directory;
	*info = file;
	if (!is_dir && (flags & fmdsf_metadata) == fmdsf_metadata)
		return fmdp_probe_file(dirfd, file), 0;
	return 0;
}


static int
fmd_scan_hier(int parent_dirfd,
	      const char *path,
	      enum FmdScanFlags flags,
	      struct FmdFile **info)
{
	assert(path);
	assert(info);

	/* XXX: add up statistics to keep track of errors */

	enum { fullpath_sz = 2048 };
	const size_t path_len = strlen(path) + 1;
	if (path_len + 10 > fullpath_sz)
		return errno = ENAMETOOLONG, -1;

	int dirfd = openat(parent_dirfd, path, O_RDONLY | O_DIRECTORY);
	if (dirfd == -1)
		return -1;
	DIR *dirp = fdopendir(dirfd);
	if (!dirp) {
		close(dirfd);	/* could lose errno */
		return -1;
	}

	struct FmdFile *rv = 0;
	{
		struct FmdFile *tail = 0;
		char fullpath[fullpath_sz];
		strcpy(fullpath, path);
		fullpath[path_len - 1] = '/';

		struct dirent *entry;
		while ((entry = readdir(dirp)) != NULL) {
			struct FmdFile *file = 0;
			if (path_len + entry->d_namlen + 1 >= sizeof fullpath)
				continue; /* path too long */
			if (entry->d_name[0] == '.' &&
			    (entry->d_name[1] == '\0' ||
			     (entry->d_name[1] == '.' &&
			      entry->d_name[2] == '\0')))
				continue; /* omit . and .. */

			strcpy(fullpath + path_len, entry->d_name);
			int res = fmd_scan_file(dirfd, fullpath, flags, &file);
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
	closedir(dirp);

	/* Breath first; time to scan directories */
	struct FmdFile *it = rv;
	while (it) {
		if (it->filetype == fmdft_directory &&
		    it->name[0] != '.') {
			struct FmdFile *children = 0;
			int res = fmd_scan_hier(dirfd, it->path, flags,
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
	close(dirfd);

	*info = rv;
	return 0;
}


int
fmd_scan(const char *location,
	 enum FmdScanFlags flags,
	 struct FmdFile **info)
{
	assert(location);
	assert(info);

	if ((flags & fmdsf_recursive) != fmdsf_recursive)
		return fmd_scan_file(AT_FDCWD, location, flags, info);
	return fmd_scan_hier(AT_FDCWD, location, flags, info);
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
