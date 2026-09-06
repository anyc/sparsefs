/*
 *  SparseFS
 *  --------
 *
 *  FUSE client that creates a sparse view of an existing filesystem
 * 
 *  Copyright 2016 Mario Kicherer <dev@kicherer.org>
 * 
 *  sparsefs is based on FilterFS by Gregor Zurowski and Kristofer Henriksson,
 *  see the copyright below.
 *
 *  FilterFS Copyright (C) 2010:
 *    Gregor Zurowski <gregor.zurowski@lunetta.net>
 *    Kristofer Henriksson <kthenriksson@gmail.com>
 * 
 *  This program can be distributed under the terms of the GNU GPLv3. See the file COPYING.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef linux
/* For pread()/pwrite() */
#define _XOPEN_SOURCE 500
#endif

#define FUSE_USE_VERSION 26

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <fuse.h>
#include <limits.h>
#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <libgen.h>
#include <wildmatch.h>
#include <ctype.h>

#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

//#define ENABLE_OUTPUT
#ifdef ENABLE_OUTPUT
#define ffs_debug(f, ...) fprintf(stdout, f, ## __VA_ARGS__)
#define ffs_info(f, ...) syslog(LOG_INFO, f, ## __VA_ARGS__)
#define ffs_error(f, ...) syslog(LOG_ERR, f, ## __VA_ARGS__)
#else
#define ffs_debug(f, ...)
#define ffs_info(f, ...)
#define ffs_error(f, ...)
#endif

int default_exclude = 0;
int debug = 0;

struct source {
	char *path;
} *sources = 0;
unsigned int n_sources = 0;

enum {
	KEY_EXCLUDE,
	KEY_INCLUDE,
	KEY_EXCLUDEFILE,
	KEY_INCLUDEFILE,
	KEY_DEFAULT_EXCLUDE,
	KEY_DEFAULT_INCLUDE,
	KEY_SOURCE,
	KEY_HELP,
	KEY_VERSION,
	KEY_KEEP_OPT
};

static struct fuse_opt ffs_opts[] = {
	FUSE_OPT_KEY("-X %s",                   KEY_EXCLUDE),
	FUSE_OPT_KEY("--exclude=%s",            KEY_EXCLUDE),
	FUSE_OPT_KEY("exclude=%s",              KEY_EXCLUDE),
	FUSE_OPT_KEY("--excludefile=%s",        KEY_EXCLUDEFILE),
	FUSE_OPT_KEY("-I %s",                   KEY_INCLUDE),
	FUSE_OPT_KEY("--include=%s",            KEY_INCLUDE),
	FUSE_OPT_KEY("include=%s",              KEY_INCLUDE),
	FUSE_OPT_KEY("-s %s",                   KEY_SOURCE),
	FUSE_OPT_KEY("source=%s",               KEY_SOURCE),
	FUSE_OPT_KEY("--source=%s",             KEY_SOURCE),
	FUSE_OPT_KEY("--includefile=%s",        KEY_INCLUDEFILE),
	FUSE_OPT_KEY("--default-exclude",       KEY_DEFAULT_EXCLUDE),
	FUSE_OPT_KEY("--default-include",       KEY_DEFAULT_INCLUDE),
	FUSE_OPT_KEY("-d",                      KEY_KEEP_OPT),
	
	FUSE_OPT_KEY("-h",            KEY_HELP),
	FUSE_OPT_KEY("--help",        KEY_HELP),
	FUSE_OPT_KEY("-V",            KEY_VERSION),
	FUSE_OPT_KEY("--version",     KEY_VERSION),
	FUSE_OPT_END
};

struct rule {
	char *pattern;
	int exclude;
	struct rule *next;
};

struct {
	struct rule *head;
	struct rule *tail;
} chain;

static int size_multiply_overflow(size_t left, size_t right)
{
	return right != 0 && left > SIZE_MAX / right;
}

/*
 * Appends a single rule to the filter chain.
 */
static int append_rule(char *pattern, int exclude)
{
	size_t pattern_length;
	struct rule *rule;

	if (!pattern)
		return -1;
	
	pattern_length = strlen(pattern);
	
	/* Strip a pair of quotation marks, but never index an empty string. */
	if (pattern_length >= 2 && pattern[0] == '"' &&
			pattern[pattern_length - 1] == '"') {
		memmove(pattern, pattern + 1, pattern_length - 2);
		pattern_length -= 2;
		pattern[pattern_length] = 0;
	}
	
	// strip trailing '/' from directories
	if (pattern_length > 0 && pattern[pattern_length - 1] == '/')
		pattern[--pattern_length] = 0;

	/* Empty rules are harmless and can be produced by a trailing ':'. */
	if (pattern_length == 0) {
		free(pattern);
		return 0;
	}

	rule = malloc(sizeof(struct rule));
	if (!rule) {
		free(pattern);
		return -1;
	}

	rule->pattern = pattern;
	
	rule->exclude = exclude;
	rule->next = NULL;
	
	if (!chain.head) {
		chain.head = rule;
		chain.tail = rule;
	} else {
		chain.tail->next = rule;
		chain.tail = rule;
	}
	
	return 0;
}

/*
 * Appends multiple rules to the filter chain.
 */
static int append_rules(char *patterns, int exclude)
{
	char *str = patterns;
	char *next;
	char *rule_pattern;
	
	while (1) {
		next = strchr(str, ':');
		if (next)
			*next = 0;

		if (*str) {
			rule_pattern = strdup(str);
			if (!rule_pattern) {
				free(patterns);
				return -1;
			}
			if (append_rule(rule_pattern, exclude) == -1) {
				free(patterns);
				return -1;
			}
		}

		if (!next)
			break;
		str = next + 1;
	}

	free(patterns);
	return 0;
}

/*
 * Append a source directory to the list
 */
static int append_source(char *source)
{
	size_t srcdir_length;
	struct source *new_sources;
	char *path;

	if (!source)
		return -1;
	if (source[0] == 0) {
		free(source);
		return 0;
	}
	
	if (n_sources == UINT_MAX ||
		size_multiply_overflow((size_t)n_sources + 1, sizeof(*sources))) {
		free(source);
		return -1;
	}

	new_sources = realloc(sources, sizeof(*sources) * (n_sources + 1));
	if (!new_sources) {
		free(source);
		return -1;
	}
	sources = new_sources;
	
	srcdir_length = strlen(source);
	if (srcdir_length > SIZE_MAX - 2) {
		free(source);
		return -1;
	}
	
	// make sure srcdir ends with a '/'
	if (source[srcdir_length-1] == '/') {
		path = strdup(source);
	} else {
		path = malloc(srcdir_length + 2);
		if (path)
			snprintf(path, srcdir_length + 2, "%s/", source);
	}
	free(source);
	if (!path)
		return -1;

	sources[n_sources].path = path;
	n_sources++;
	
	return 0;
}

/*
 * check if string only contains whitespaces and calculate length
 */
void checkString(const char *s, size_t *length, char *empty) {
	*length = 0;
	*empty = 1;
	while (*s != '\0') {
		if (*empty && !isspace((unsigned char)*s))
			*empty = 0;
		s++;
		(*length)++;
	}
}

/*
 * read rules from file
 */
static int parse_file(const char *filename, int exclude)
{
	FILE *f;
	char line[PATH_MAX];
	char empty;
	size_t len;
	
	f = fopen(filename, "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			checkString(line, &len, &empty);
			if (len == sizeof(line) - 1 && line[len - 1] != '\n' && !feof(f)) {
				fclose(f);
				return -ENAMETOOLONG;
			}
			
			if (empty)
				continue;
			if (line[0] == '#')
				continue;
			
			// remove trailing newlines
			if (line[len-1] == '\n')
				line[len-1] = 0;
			
			if (append_rule(strdup(line), exclude) == -1) {
				fclose(f);
				return -ENOMEM;
			}
		}
		
		if (ferror(f)) {
			fclose(f);
			return -EIO;
		}
		fclose(f);
	} else {
		ffs_error("cannot open file \"%s\"\n", filename);
		return -errno;
	}

	return 0;
}

/*
 * Checks whether the provided path should be excluded.
 */
static int exclude_chroot_path(const char *path)
{
	struct rule *curr_rule;
	size_t len;
	unsigned int i;
	
	len = strlen(path);
	
	// always allow access to the srcdir itself (although it might appear empty)
	for (i=0; i < n_sources; i++) {
		if (strcmp(path, sources[i].path) == 0)
			return 0;
	}
	
	// always accept "." and ".." directories
	if (len >= 2 && strcmp(&path[len-2], "/.") == 0)
		return 0;
	
	if (len >= 3 && strcmp(&path[len-3], "/..") == 0)
		return 0;
	
	curr_rule = chain.head;
	while (curr_rule) {
		if (wildmatch(curr_rule->pattern, path, WM_PATHNAME, NULL) == WM_MATCH) {
			break;
		}
		curr_rule = curr_rule->next;
	}
	
	if (curr_rule)
		return curr_rule->exclude;
	else
		return default_exclude;
}

/* Build a source path without silently truncating it. */
static int build_path(char *realpath, size_t realpath_size,
			const char *source, const char *fuse_path)
{
	int length;

	if (!fuse_path || fuse_path[0] != '/')
		return -EINVAL;

	length = snprintf(realpath, realpath_size, "%s%s", source,
			&fuse_path[1]);
	if (length < 0)
		return -EINVAL;
	if ((size_t)length >= realpath_size)
		return -ENAMETOOLONG;

	return 0;
}

/*
 * Build a real path and check if it should be excluded. Invalid or too-long
 * paths are treated as excluded rather than being passed to the filesystem.
 */
static int exclude_path(char *realpath, size_t realpath_size, const char *fuse_path)
{
	unsigned int i;
	int exclude;
	int result;
	char parent[PATH_MAX];
	char *slash;
	
	exclude = 1;
	for (i=0; i < n_sources; i++) {
		result = build_path(realpath, realpath_size, sources[i].path, fuse_path);
		if (result < 0) {
			realpath[0] = 0;
			return 1;
		}
		
		// only check this path if it exists in this source
		if (access(realpath, F_OK) != -1) {
			exclude = exclude_chroot_path(realpath);
		} else {
			/* New filesystem objects are selected by their parent source. */
			if (strlen(realpath) >= sizeof(parent))
				continue;
			strcpy(parent, realpath);
			slash = strrchr(parent, '/');
			if (!slash)
				continue;
			if (slash == parent)
				slash[1] = 0;
			else
				slash[0] = 0;
			if (access(parent, F_OK) == -1 || exclude_chroot_path(parent))
				continue;
			exclude = exclude_chroot_path(realpath);
		}

		/* If this path is included, use this source. */
		if (!exclude)
			break;
	}
	
	return exclude;
}

/*
 * Checks if str1 begins with str2. If so, returns a pointer to the end of
 * the match. Otherwise, returns null.
 */
static const char *str_consume(const char *str1, const char *str2)
{
	if (strncmp(str1, str2, strlen(str2)) == 0) {
		return str1 + strlen(str2);
	}
	
	return 0;
}



/*
 * FUSE callback operations
 */

static int ffs_getattr(const char *path, struct stat *stbuf)
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("getattr: path %s (expanded %s), exclude %s\n", path,
			realpath, exclude ? "y" : "n");
	
	if (exclude < 0)
		return exclude;
	if (exclude)
		return -ENOENT;
	
	int res;
	res = lstat(realpath, stbuf);
	if (res == -1)
		return -errno;
	
	return 0;
}

static int ffs_access(const char *path, int mask)
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("access: path %s (expanded %s), exclude %s\n", path,
			realpath, exclude ? "y" : "n");
	
	if (exclude < 0)
		return exclude;
	if (exclude)
		return -ENOENT;
	
	int res;
	res = access(realpath, mask);
	if (res == -1)
		return -errno;
	
	return 0;
}

static int ffs_readlink(const char *path, char *buf, size_t size)
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("readlink: path %s (expanded %s), exclude %s\n", path,
			realpath, exclude ? "y" : "n");
	
	if (exclude < 0)
		return exclude;
	if (exclude)
		return -ENOENT;
	if (size == 0)
		return -EINVAL;
	
	int res;
	res = readlink(realpath, buf, size - 1);
	if (res == -1)
		return -errno;
	
	buf[res] = '\0';
	return 0;
}

static int ffs_readdir_helper(unsigned int source_idx, char *realpath, const char *path, void *buf, fuse_fill_dir_t filler) {
	char subpath[PATH_MAX];
	DIR *dp;
	struct dirent *de;
	int exclude;
	unsigned int i;
	
	dp = opendir(realpath);
	if (dp == NULL)
		return -errno;
	
	while ((de = readdir(dp)) != NULL) {
		char skip = 0;
		
		// check if one of the previous sources already added an entity with this name
		for (i=0; i < source_idx; i++) {
			if (snprintf(subpath, PATH_MAX, "%s%s%s%s", sources[i].path,
					&path[1], path[1] == 0 ? "":"/", de->d_name) >= PATH_MAX)
				continue;
			if (access(subpath, F_OK) != -1) {
				exclude = exclude_chroot_path(subpath);
				
				// was entity in previous source excluded?
				if (!exclude) {
					skip = 1;
					break;
				}
			}
		}

			if (skip)
				continue;

			if (snprintf(subpath, PATH_MAX, "%s%s%s", realpath,
					path[1] == 0 ? "":"/", de->d_name) >= PATH_MAX)
				continue;
		
		exclude = exclude_chroot_path(subpath);
		
		ffs_debug("readdir[2]: path %s (expanded %s), exclude: %s\n",
				  de->d_name, subpath, exclude ? "y" : "n");
		
		if (exclude)
			continue;
		
		struct stat st;
		if (lstat(subpath, &st) == -1)
			continue;
		if (filler(buf, de->d_name, &st, 0))
			break;
	}
	
	closedir(dp);
	
	return 0;
}

static int ffs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
				   off_t offset, struct fuse_file_info *fi)
{
	char realpath[PATH_MAX];
	unsigned int i;
	int r, exclude;
	(void)offset;
	(void)fi;
	// If we have to list the root of the fuse directory, we add the root entries
	// from all sources. Else, we just show the entries from the 
	if (!strcmp(path, "/")) {
		for (i=0; i < n_sources; i++) {
			r = ffs_readdir_helper(i, sources[i].path, path, buf, filler);
			if (r)
				return r;
		}
	} else {
		for (i=0; i < n_sources; i++) {
			if (build_path(realpath, PATH_MAX, sources[i].path, path) < 0)
				continue;
			
			if (access(realpath, F_OK) != -1) {
				exclude = exclude_chroot_path(realpath);
			} else {
				continue;
			}
			
			if (!exclude) {
				r = ffs_readdir_helper(i, realpath, path, buf, filler);
				if (r)
					return r;
			}
		}
	}
	
	return 0;
}

static int ffs_mknod(const char *path, mode_t mode, dev_t rdev)
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("mknod: path %s (expanded %s), exclude %s\n", path, realpath,
			exclude ? "y" : "n");
	
	if (exclude)
		return -ENOENT;
	
	int res;
	
	/* On Linux this could just be 'mknod(path, mode, rdev)' but this
	 *       is more portable */
	if (S_ISREG(mode)) {
		res = open(realpath, O_CREAT | O_EXCL | O_WRONLY, mode);
		if (res >= 0)
			res = close(res);
	} else if (S_ISFIFO(mode))
		res = mkfifo(realpath, mode);
	else
		res = mknod(realpath, mode, rdev);
	if (res == -1)
		return -errno;
	
	return 0;
}

static int ffs_mkdir(const char *path, mode_t mode)
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("mkdir: path %s (expanded %s), exclude %s\n", path, realpath,
			exclude ? "y" : "n");
	
	if (exclude)
		return -ENOENT;
	
	int res;
	res = mkdir(realpath, mode);
	if (res == -1)
		return -errno;
	
	return 0;
}

static int ffs_unlink(const char *path)
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("unlink: path %s (expanded %s), exclude %s\n", path,
			realpath, exclude ? "y" : "n");
	
	if (exclude)
		return -ENOENT;
	
	int res;
	res = unlink(realpath);
	if (res == -1)
		return -errno;
	
	return 0;
}

static int ffs_rmdir(const char *path)
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("rmdir: path %s (expanded %s), exclude %s\n", path, realpath,
			exclude ? "y" : "n");
	
	if (exclude)
		return -ENOENT;
	
	int res;
	res = rmdir(realpath);
	if (res == -1)
		return -errno;
	
	return 0;
}

static int ffs_symlink(const char *from, const char *to)
{
	char xto[PATH_MAX];
	char target[PATH_MAX];
	const char *symlink_target = from;
	unsigned int i;
	
	int exclude_to = exclude_path(xto, PATH_MAX, to);
	
	ffs_debug("symlink: target %s; to %s (expanded %s), exclude %s\n",
			from, to, xto, exclude_to ? "y": "n");
	
	if (exclude_to)
		return -ENOENT;

	/* Absolute FUSE targets need the backing source prefix. */
	if (from[0] == '/') {
		for (i = 0; i < n_sources; i++) {
			if (strncmp(xto, sources[i].path, strlen(sources[i].path)) == 0) {
				if (snprintf(target, sizeof(target), "%s%s", sources[i].path,
						&from[1]) >= (int)sizeof(target))
					return -ENAMETOOLONG;
				symlink_target = target;
				break;
			}
		}
	}
	
	int res;
	res = symlink(symlink_target, xto);
	if (res == -1)
		return -errno;
	
	return 0;
}

static int ffs_rename(const char *from, const char *to)
{
	char xfrom[PATH_MAX];
	char xto[PATH_MAX];
	
	int exclude_from = exclude_path(xfrom, PATH_MAX, from);
	int exclude_to = exclude_path(xto, PATH_MAX, to);
	
	ffs_debug("rename: from %s (expanded %s), exclude %s; to %s"
			" (expanded %s), exclude %s\n", from, xfrom,
			exclude_from ? "y" : "n", to, xto, exclude_to ? "y": "n");
	
	if (exclude_from || exclude_to)
		return -ENOENT;
	
	int res;
	res = rename(xfrom, xto);
	if (res == -1)
		return -errno;
	
	return 0;
}

static int ffs_link(const char *from, const char *to)
{
	char xfrom[PATH_MAX];
	char xto[PATH_MAX];
	
	int exclude_from = exclude_path(xfrom, PATH_MAX, from);
	int exclude_to = exclude_path(xto, PATH_MAX, to);
	
	ffs_debug("link: from %s (expanded %s), exclude %s; to %s"
			" (expanded %s), exclude %s\n", from, xfrom,
			exclude_from ? "y" : "n", to, xto, exclude_to ? "y": "n");
	
	if (exclude_from || exclude_to)
		return -ENOENT;
	
	int res;
	res = link(xfrom, xto);
	if (res == -1)
		return -errno;
	
	return 0;
}

static int ffs_chmod(const char *path, mode_t mode)
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("chmod: path %s (expanded %s), exclude %s\n", path, realpath,
			exclude ? "y" : "n");
	
	if (exclude)
		return -ENOENT;
	
	int res;
	res = chmod(realpath, mode);
	if (res == -1)
		return -errno;
	
	return 0;
}

static int ffs_chown(const char *path, uid_t uid, gid_t gid)
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("chown: path %s (expanded %s), exclude %s\n", path, realpath,
			exclude ? "y" : "n");
	
	if (exclude)
		return -ENOENT;
	
	int res;
	res = lchown(realpath, uid, gid);
	if (res == -1)
		return -errno;
	
	return 0;
}

static int ffs_truncate(const char *path, off_t size)
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("truncate: path %s (expanded %s), exclude %s\n", path,
			realpath, exclude ? "y" : "n");
	
	if (exclude)
		return -ENOENT;
	
	int res;
	res = truncate(realpath, size);
	if (res == -1)
		return -errno;
	
	return 0;
}

static int ffs_utimens(const char *path, const struct timespec ts[2])
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("utimens: path %s (expanded %s), exclude %s\n", path,
			realpath, exclude ? "y" : "n");
	
	if (exclude)
		return -ENOENT;
	
	int res;
	struct timeval tv[2];
	
	tv[0].tv_sec = ts[0].tv_sec;
	tv[0].tv_usec = ts[0].tv_nsec / 1000;
	tv[1].tv_sec = ts[1].tv_sec;
	tv[1].tv_usec = ts[1].tv_nsec / 1000;
	
	res = utimes(realpath, tv);
	if (res == -1)
		return -errno;
	
	return 0;
}

static int ffs_open(const char *path, struct fuse_file_info *fi)
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("open: path %s (expanded %s), exclude %s\n", path, realpath,
			exclude ? "y" : "n");
	
	if (exclude)
		return -ENOENT;
	
	int res;
	res = open(realpath, fi->flags);
	if (res == -1)
		return -errno;

	/* Encode fd + 1 so fd 0 is not confused with an unset handle. */
	fi->fh = (uint64_t)res + 1;
	return 0;
}

static int ffs_read(const char *path, char *buf, size_t size, off_t offset,
				struct fuse_file_info *fi)
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("read: path %s (expanded %s), exclude %s\n", path, realpath,
			exclude ? "y" : "n");
	
	if (exclude)
		return -ENOENT;
	
	int fd;
	int close_fd = 0;
	int res;
	
	if (fi->fh) {
		fd = (int)(fi->fh - 1);
	} else {
		fd = open(realpath, O_RDONLY);
		if (fd == -1)
			return -errno;
		close_fd = 1;
	}
	
	res = pread(fd, buf, size, offset);
	if (res == -1)
		res = -errno;
	
	if (close_fd)
		close(fd);
	return res;
}

static int ffs_write(const char *path, const char *buf, size_t size,
					 off_t offset, struct fuse_file_info *fi)
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("write: path %s (expanded %s), exclude %s\n", path, realpath,
			exclude ? "y" : "n");
	
	if (exclude)
		return -ENOENT;
	
	int fd;
	int close_fd = 0;
	int res;
	
	if (fi->fh) {
		fd = (int)(fi->fh - 1);
	} else {
		fd = open(realpath, O_WRONLY);
		if (fd == -1)
			return -errno;
		close_fd = 1;
	}
	
	res = pwrite(fd, buf, size, offset);
	if (res == -1)
		res = -errno;
	
	if (close_fd)
		close(fd);
	return res;
}

static int ffs_statfs(const char *path, struct statvfs *stbuf)
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("statfs: path %s (expanded %s), exclude %s\n", path,
			realpath, exclude ? "y" : "n");
	
	if (exclude)
		return -ENOENT;
	
	int res;
	res = statvfs(realpath, stbuf);
	if (res == -1)
		return -errno;
	
	return 0;
}

static int ffs_release(const char *path, struct fuse_file_info *fi)
{
	/* 
	 * Just a stub.  This method is optional and can safely be left
	 *       unimplemented
	 */
	
	(void)path;
	if (fi->fh)
		close((int)(fi->fh - 1));
	fi->fh = 0;
	return 0;
}

static int ffs_fsync(const char *path, int isdatasync,
				 struct fuse_file_info *fi)
{
	/* 
	 * Just a stub.  This method is optional and can safely be left
	 *       unimplemented
	 */
	
	int res;
	(void)path;
	if (!fi->fh)
		return -EBADF;
	res = isdatasync ? fdatasync((int)(fi->fh - 1)) :
			fsync((int)(fi->fh - 1));
	return res == -1 ? -errno : 0;
}

#ifdef HAVE_SETXATTR

/* xattr operations are optional */
static int ffs_setxattr(const char *path, const char *name, const char *value,
				    size_t size, int flags)
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("setxattr: path %s (expanded %s), exclude %s\n", path,
			realpath, exclude ? "y" : "n");
	
	if (exclude)
		return -ENOENT;
	
	int res = lsetxattr(realpath, name, value, size, flags);
	if (res == -1)
		return -errno;
	return 0;
}

static int ffs_getxattr(const char *path, const char *name, char *value,
				    size_t size)
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("getxattr: path %s (expanded %s), exclude %s\n", path,
			realpath, exclude ? "y" : "n");
	
	if (exclude)
		return -ENOENT;
	
	int res = lgetxattr(realpath, name, value, size);
	if (res == -1)
		return -errno;
	return res;
}

static int ffs_listxattr(const char *path, char *list, size_t size)
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("listxattr: path %s (expanded %s), exclude %s\n", path,
			realpath, exclude ? "y" : "n");
	
	if (exclude)
		return -ENOENT;
	
	int res = llistxattr(realpath, list, size);
	if (res == -1)
		return -errno;
	return res;
}

static int ffs_removexattr(const char *path, const char *name)
{
	char realpath[PATH_MAX];
	
	int exclude = exclude_path(realpath, PATH_MAX, path);
	
	ffs_debug("removexattr: path %s (expanded %s), exclude %s\n", path,
			realpath, exclude ? "y" : "n");
	
	if (exclude)
		return -ENOENT;
	
	int res = lremovexattr(realpath, name);
	if (res == -1)
		return -errno;
	return 0;
}

#endif /* HAVE_SETXATTR */

static struct fuse_operations ffs_oper = {
	.getattr    = ffs_getattr,
	.access     = ffs_access,
	.readlink   = ffs_readlink,
	.readdir    = ffs_readdir,
	.mknod      = ffs_mknod,
	.mkdir      = ffs_mkdir,
	.symlink    = ffs_symlink,
	.unlink     = ffs_unlink,
	.rmdir      = ffs_rmdir,
	.rename     = ffs_rename,
	.link       = ffs_link,
	.chmod      = ffs_chmod,
	.chown      = ffs_chown,
	.truncate   = ffs_truncate,
	.utimens    = ffs_utimens,
	.open       = ffs_open,
	.read       = ffs_read,
	.write      = ffs_write,
	.statfs     = ffs_statfs,
	.release    = ffs_release,
	.fsync      = ffs_fsync,
#ifdef HAVE_SETXATTR
	.setxattr   = ffs_setxattr,
	.getxattr   = ffs_getxattr,
	.listxattr  = ffs_listxattr,
	.removexattr    = ffs_removexattr,
#endif
};

static void usage(const char *progname)
{
	fprintf(stderr,
		"\nusage: %s [options] mountpoint\n"
		"\n"
		"general options:\n"
		"    -o opt,[opt...]        mount options\n"
		"    -h   --help            print help\n"
		"    -V   --version         print version\n"
		"\n"
		"SparseFS options:\n"
		"    -s <source dir>                        source directory\n"
		"    -X, --exclude=<pattern>[:<pattern>...] patterns for files to be excluded\n"
		"    -I, --include=<pattern>[:<pattern>...] patterns for files to be included\n"
		"    --excludefile=<filename>               file with one exclude pattern in each line\n"
		"    --includefile=<filename>               file with one include pattern in each line\n"
		"    --default-exclude                      exclude unmatched items (default)\n"
		"    --default-include                      include unmatched items\n"
		"\n", progname);
}

static int ffs_opt_proc(void *data, const char *arg, int key,
				    struct fuse_args *outargs)
{
	const char *str;
	(void)data;
	
	switch(key) {
		case KEY_SOURCE:
			if (!(str = str_consume(arg, "--source="))
				&& !(str = str_consume(arg, "source="))
				&& !(str = str_consume(arg, "-s")))
				return -1;
			
			if (strlen(str) > 0 && append_source(strdup(str)) < 0)
				return -1;
			
			return 0;
			
		case KEY_EXCLUDE:
			if (!(str = str_consume(arg, "--exclude="))
				&& !(str = str_consume(arg, "exclude="))
				&& !(str = str_consume(arg, "-X")))
				return -1;
			
			if (strlen(str) > 0 && append_rules(strdup(str), 1) < 0)
				return -1;
			
			return 0;
			
		case KEY_EXCLUDEFILE:
			if (!(str = str_consume(arg, "--excludefile=")))
				return -1;
			
			if (parse_file(str, 1) < 0)
				return -1;
			
			return 0;
			
		case KEY_INCLUDE:
			if (!(str = str_consume(arg, "--include="))
				&& !(str = str_consume(arg, "include="))
				&& !(str = str_consume(arg, "-I")))
				return -1;
			
			/* See comment for KEY_EXCLUDE above. */
			if (strlen(str) > 0 && append_rules(strdup(str), 0) < 0)
				return -1;
			
			return 0;
			
		case KEY_INCLUDEFILE:
			if (!(str = str_consume(arg, "--includefile=")))
				return -1;
			
			if (parse_file(str, 0) < 0)
				return -1;
			
			return 0;
			
		case KEY_DEFAULT_EXCLUDE:
			default_exclude = 1;
			return 0;
			
		case KEY_DEFAULT_INCLUDE:
			default_exclude = 0;
			return 0;
			
		case KEY_HELP:
			usage(outargs->argv[0]);
			fuse_opt_add_arg(outargs, "-ho");
			fuse_main(outargs->argc, outargs->argv, &ffs_oper, NULL);
			exit(1);
			
		case KEY_VERSION:
			printf("SparseFS version %s\n", "0.2");
			fuse_opt_add_arg(outargs, "--version");
			fuse_main(outargs->argc, outargs->argv, &ffs_oper, NULL);
			exit(0);
			
		case KEY_KEEP_OPT:
			debug = 1;
			return 1;
	}
	
	return 1;
}

int main(int argc, char *argv[])
{
	unsigned int i;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	
	if (fuse_opt_parse(&args, NULL, ffs_opts, ffs_opt_proc)) {
		fprintf(stderr, "error: general error while parsing options.\n");
		usage(argv[0]);
		return 1;
	}
	
	if (n_sources == 0) {
		fprintf(stderr, "error: no source directory specified.\n");
		usage(argv[0]);
		return 1;
	}
	
	/* Log to the screen if debug is enabled. */
	openlog("sparsefs", debug ? LOG_PERROR : 0, LOG_USER);
	
	for (i=0; i < n_sources; i++) {
		if (sources[i].path[0] != '/') {
			fprintf(stderr, "error: source directory must be an absolute path.\n");
			usage(argv[0]);
			return 1;
		}
		
		struct stat st;
		if (stat(sources[i].path, &st) != 0 || !S_ISDIR(st.st_mode)) {
			fprintf(stderr, "error: source directory path does not exist or is not a directory.\n");
			usage(argv[0]);
			return 1;
		}
		
		ffs_info("source dir: %s\n", sources[i].path);
	}
	
	/* Log startup information */
	ffs_info("default action: %s\n", default_exclude ? "exclude" : "include");
	
#ifdef ENABLE_OUTPUT
	struct rule *curr_rule = chain.head;
	unsigned int rule_index = 1;
	while (curr_rule) {
		ffs_info("filter %d: %s %s\n", rule_index++,
				curr_rule->exclude ? "exclude" : "include",
				curr_rule->pattern);
		curr_rule = curr_rule->next;
	}
#endif
	
	umask(0);
	int ret = fuse_main(args.argc, args.argv, &ffs_oper, NULL);
	
	return ret;
}
