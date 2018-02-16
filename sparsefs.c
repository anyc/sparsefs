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

// TODO find a way to determine a good number
#define HT_LENGTH 100
struct rule *ht[HT_LENGTH] = {0};



unsigned long calc_hash(const char *hstr)
{
	unsigned long hash = 5381;
	const unsigned char *str = hstr;
	int c;
	
	while (c = *str++)
		hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
	
	return hash;
}

struct rule *getRuleByHash(const char *s)
{
	unsigned long hash;
	struct rule *e;
	
	hash = calc_hash(s);
	
	e = ht[hash % HT_LENGTH];
	for (; e && strcmp(e->pattern, s); e = e->next) {}
	
	return e;
}

/*
 * Appends a single rule to the filter chain.
 */
static int append_rule(char *pattern, int exclude)
{
	size_t pattern_length;
	char *quotmark;
	unsigned long hash;
	struct rule *rule, *ht_head;
	
	rule = malloc(sizeof(struct rule));
	if (!rule)
		return -1;
	
	rule->pattern = pattern;
	pattern_length = strlen(pattern);
	
	// strip quotation marks at start and end
	if (pattern[0] == '"' || pattern[pattern_length-1] == '"') {
		rule->pattern = &pattern[1];
		pattern[pattern_length-1] = 0;
		pattern_length--;
	}
	
	// strip trailing '/' from directories
	if (pattern[pattern_length-1] == '/')
		pattern[pattern_length-1] = 0;
	
	rule->exclude = exclude;
	rule->next = NULL;
	
	// if pattern contains wildcards do not add it to the hashtable
	if (strpbrk(pattern, "*?")) {
		if (!chain.head) {
			chain.head = rule;
			chain.tail = rule;
		}
		else {
			chain.tail->next = rule;
			chain.tail = rule;
		}
	} else {
		hash = calc_hash(rule->pattern);
		ht_head = ht[hash % HT_LENGTH];
		
		if (ht_head) {
			for (; ht_head->next; ht_head = ht_head->next) {}
			ht_head->next = rule;
		} else {
			ht[hash % HT_LENGTH] = rule;
		}
	}
	
	return 0;
}

/*
 * Appends multiple rules to the filter chain.
 */
static int append_rules(char *patterns, int exclude)
{
	char *str = patterns;
	
	while (1) {
		if (append_rule(str, exclude) == -1)
			return -1;
		
		if (!(str = strchr(str, ':')))
			break;
		
		*str = '\0';
		str++;
	}
	
	return 0;
}

/*
 * Append a source directory to the list
 */
static int append_source(char *source)
{
	size_t srcdir_length;
	
	n_sources += 1;
	sources = (struct source*) realloc(sources, sizeof(struct source) * n_sources);
	
	srcdir_length = strlen(source);
	
	// make sure srcdir ends with a '/'
	if (source[srcdir_length-1] == '/') {
		sources[n_sources-1].path = strdup(source);
	} else {
		sources[n_sources-1].path = (char*) malloc(srcdir_length + 2);
		sprintf(sources[n_sources-1].path, "%s/", source);
	}
	
	return 0;
}

/*
 * check if string only contains whitespaces and calculate length
 */
void checkString(const char *s, size_t *length, char *empty) {
	*length = 0;
	*empty = 1;
	while (*s != '\0') {
		if (*empty && !isspace(*s))
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
			
			if (empty)
				continue;
			if (line[0] == '#')
				continue;
			
			// remove trailing newlines
			if (line[len-1] == '\n')
				line[len-1] = 0;
			
			append_rule(strdup(line), exclude);
		}
		
		fclose(f);
	} else {
		ffs_error("cannot open file \"%s\"\n", filename);
	}
}

/*
 * Checks whether the provided path should be excluded.
 */
static int exclude_chroot_path(const char *path)
{
	struct stat st, symt;
	struct rule *curr_rule;
	size_t len;
	unsigned int i;
	
	lstat(path, &st);
	len = strlen(path);
	
	// always allow access to the srcdir itself (although it might appear empty)
	for (i=0; i < n_sources; i++) {
		if (strcmp(path, sources[i].path) == 0)
			return 0;
	}
	
	// always accept "." and ".." directories
	if (strcmp(&path[len-2], "/.") == 0)
		return 0;
	
	if (strcmp(&path[len-3], "/..") == 0)
		return 0;
	
	// if pattern contains wildcards do not look in the hash table
	// TODO consider escaped characters
	if (strpbrk(path, "*?"))
		curr_rule = 0;
	else
		curr_rule = getRuleByHash(path);
	
	if (!curr_rule) {
		curr_rule = chain.head;
		while (curr_rule) {
			if (wildmatch(curr_rule->pattern, path, WM_PATHNAME, NULL) == WM_MATCH) {
				break;
			}
			curr_rule = curr_rule->next;
		}
	}
	
	if (curr_rule)
		return curr_rule->exclude;
	else
		return default_exclude;
}

/*
 * build real path and check if it should be excluded
 */
static int exclude_path(char *realpath, size_t realpath_size, const char *fuse_path)
{
	unsigned int i;
	int exclude;
	
	exclude = 1;
	for (i=0; i < n_sources; i++) {
		// concatenate strings and strip starting '/' from $fuse_path
		snprintf(realpath, realpath_size, "%s%s", sources[i].path, &fuse_path[1]);
		
		// only check this path if it exists in this source
		if (access(realpath, F_OK) != -1) {
			exclude = exclude_chroot_path(realpath);
			
			// if this path is included, use it
			if (!exclude)
				break;
		}
	}
	
	return exclude;
}

/*
 * Checks if str1 begins with str2. If so, returns a pointer to the end of
 * the match. Otherwise, returns null.
 */
static const char *str_consume(const char *str1, char *str2)
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
	
	if (exclude)
		return -ENOENT;
	
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
			snprintf(subpath, PATH_MAX, "%s%s%s%s", sources[i].path, &path[1], path[1] == 0 ? "":"/", de->d_name);
			fprintf(stderr, "rdir %s\n", subpath);
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
		
		snprintf(subpath, PATH_MAX, "%s%s%s", realpath, path[1] == 0 ? "":"/", de->d_name);
		
		exclude = exclude_chroot_path(subpath);
		
		ffs_debug("readdir[2]: path %s (expanded %s), exclude: %s\n",
				  de->d_name, subpath, exclude ? "y" : "n");
		
		if (exclude)
			continue;
		
		struct stat st;
		memset(&st, 0, sizeof(st));
		st.st_ino = de->d_ino;
		st.st_mode = de->d_type << 12;
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
	int i, r, exclude;
	ffs_debug("readdir[1]: path %s (expanded %s), exclude: %s\n", path,
			realpath, exclude ? "y" : "n");
	
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
			snprintf(realpath, PATH_MAX, "%s%s", sources[i].path, &path[1]);
			
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
	char xfrom[PATH_MAX];
	char xto[PATH_MAX];
	
	int exclude_from = exclude_path(xfrom, PATH_MAX, from);
	int exclude_to = exclude_path(xto, PATH_MAX, to);
	
	ffs_debug("symlink: from %s (expanded %s), exclude %s; to %s"
			" (expanded %s), exclude %s\n", from, xfrom,
			exclude_from ? "y" : "n", to, xto, exclude_to ? "y": "n");
	
	if (exclude_from || exclude_to)
		return -ENOENT;
	
	int res;
	res = symlink(from, xto);
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
	
	close(res);
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
	int res;
	
	fd = open(realpath, O_RDONLY);
	if (fd == -1)
		return -errno;
	
	res = pread(fd, buf, size, offset);
	if (res == -1)
		res = -errno;
	
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
	
	ffs_debug("write: path %s (expanded %s)\n", path, realpath);
	
	if (exclude)
		return -ENOENT;
	
	int fd;
	int res;
	
	fd = open(realpath, O_WRONLY);
	if (fd == -1)
		return -errno;
	
	res = pwrite(fd, buf, size, offset);
	if (res == -1)
		res = -errno;
	
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
	
	return 0;
}

static int ffs_fsync(const char *path, int isdatasync,
				 struct fuse_file_info *fi)
{
	/* 
	 * Just a stub.  This method is optional and can safely be left
	 *       unimplemented
	 */
	
	return 0;
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
	
	switch(key) {
		case KEY_SOURCE:
			if (!(str = str_consume(arg, "--source="))
				&& !(str = str_consume(arg, "source="))
				&& !(str = str_consume(arg, "-s")))
				return -1;
			
			if (strlen(str) > 0)
				append_source(strdup(str));
			
			return 0;
			
		case KEY_EXCLUDE:
			if (!(str = str_consume(arg, "--exclude="))
				&& !(str = str_consume(arg, "exclude="))
				&& !(str = str_consume(arg, "-X")))
				return -1;
			
			/*
			 * Not actually a memory leak. We don't want this memory
			 * deallocated until program exit.
			 */
			if (strlen(str) > 0)
				append_rules(strdup(str), 1);
			
			return 0;
			
		case KEY_EXCLUDEFILE:
			if (!(str = str_consume(arg, "--excludefile=")))
				return -1;
			
			parse_file(str, 1);
			
			return 0;
			
		case KEY_INCLUDE:
			if (!(str = str_consume(arg, "--include="))
				&& !(str = str_consume(arg, "include="))
				&& !(str = str_consume(arg, "-I")))
				return -1;
			
			/* See comment for KEY_EXCLUDE above. */
			if (strlen(str) > 0)
				append_rules(strdup(str), 0);
			
			return 0;
			
		case KEY_INCLUDEFILE:
			if (!(str = str_consume(arg, "--includefile=")))
				return -1;
			
			parse_file(str, 0);
			
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
	
	struct rule *curr_rule = chain.head;
	i = 1;
	while (curr_rule) {
		ffs_info("filter %d: %s %s\n", i++,
				curr_rule->exclude ? "exclude" : "include",
				curr_rule->pattern);
		curr_rule = curr_rule->next;
	}
	
	umask(0);
	int ret = fuse_main(args.argc, args.argv, &ffs_oper, NULL);
	
	return ret;
}
