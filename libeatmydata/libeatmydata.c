/* BEGIN LICENSE
 * Copyright (C) 2008-2014 Stewart Smith <stewart@flamingspork.com>
 * This program is free software: you can redistribute it and/or modify it 
 * under the terms of the GNU General Public License version 3, as published 
 * by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranties of 
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR 
 * PURPOSE.  See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along 
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 * END LICENSE */

#include "config.h"
#include "libeatmydata/visibility.h"

#undef _FILE_OFFSET_BITS // Hack to get open and open64 on 32bit
#undef __USE_FILE_OFFSET64
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <pthread.h>
#include <sys/mman.h>

/* 
#define CHECK_FILE "/tmp/eatmydata"
*/

#ifndef __APPLE__

/*
 * Mac OS X 10.7 doesn't declare fdatasync().
 */
#if defined HAVE_DECL_FDATASYNC && !HAVE_DECL_FDATASYNC
int fdatasync(int fd);
#endif

typedef int (*libc_open_t)(const char*, int, ...);
typedef int (*libc_open64_t)(const char*, int, ...);
typedef int (*libc_fsync_t)(int);
typedef int (*libc_sync_t)(void);
typedef int (*libc_fdatasync_t)(int);
typedef int (*libc_msync_t)(void*, size_t, int);
#ifdef HAVE_SYNC_FILE_RANGE
typedef int (*libc_sync_file_range_t)(int, off64_t, off64_t, unsigned int);
#endif

static libc_open_t libc_open= NULL;
static libc_open64_t libc_open64= NULL;
static libc_fsync_t libc_fsync= NULL;
static libc_sync_t libc_sync= NULL;
static libc_fdatasync_t libc_fdatasync= NULL;
static libc_msync_t libc_msync= NULL;
#ifdef HAVE_SYNC_FILE_RANGE
static libc_sync_file_range_t libc_sync_file_range= NULL;
#endif

#define ASSIGN_DLSYM_OR_DIE(name)			\
        libc_##name = (libc_##name##_##t)(intptr_t)dlsym(RTLD_NEXT, #name);			\
        if (!libc_##name || dlerror())				\
                _exit(1);

#define ASSIGN_DLSYM_IF_EXIST(name)			\
        libc_##name = (libc_##name##_##t)(intptr_t)dlsym(RTLD_NEXT, #name);			\
						   dlerror();


static int initing = 0;

void __attribute__ ((constructor)) eatmydata_init(void);

void __attribute__ ((constructor)) eatmydata_init(void)
{
	initing = 1;
	ASSIGN_DLSYM_OR_DIE(open);
	ASSIGN_DLSYM_OR_DIE(open64);
	ASSIGN_DLSYM_OR_DIE(fsync);
	ASSIGN_DLSYM_OR_DIE(sync);
	ASSIGN_DLSYM_OR_DIE(fdatasync);
	ASSIGN_DLSYM_OR_DIE(msync);
#ifdef HAVE_SYNC_FILE_RANGE
	ASSIGN_DLSYM_IF_EXIST(sync_file_range);
#endif
	initing = 0;
}
#define REAL(x) (*libc_##x)
#define EATMYDATA(x) EATMYDATA_API x
#else
const int initing = 0;
static const struct {void *from; void *to;} interposers[] __attribute__((used)) __attribute((section("__DATA, __interpose"))) = {
	{eatmydata_open, open},
	{eatmydata_open64, open64},
	{eatmydata_fsync, fsync},
	{eatmydata_sync, sync},
	{eatmydata_fdatasync, fdatasync},
	{eatmydata_msync, msync},
};
void eatmydata_init(void) {}
#define REAL(x) (x)
#define EATMYDATA(x) eatmydata_ ## x
#endif

static int eatmydata_is_hungry(void)
{
	/* Init here, as it is called before any libc functions */
	if(!libc_open)
		eatmydata_init();

#ifdef CHECK_FILE
	static struct stat buf;
	int old_errno, stat_ret;

	old_errno= errno;
	stat_ret= stat(CHECK_FILE, &buf);
	errno= old_errno;

	/* Treat any error as if file doesn't exist, for safety */
	return !stat_ret;
#else
	/* Always hungry! */
	return 1;
#endif
}

int EATMYDATA(fsync)(int fd)
{
	if (eatmydata_is_hungry()) {
		pthread_testcancel();
		errno= 0;
		return 0;
	}

	return REAL(fsync)(fd);
}

/* no errors are defined for this function */
void EATMYDATA(sync)(void)
{
	if (eatmydata_is_hungry()) {
		return;
	}

	REAL(sync)();
}

int EATMYDATA(open)(const char* pathname, int flags, ...)
{
	va_list ap;
	mode_t mode;

	va_start(ap, flags);
#if SIZEOF_MODE_T < SIZEOF_INT
	mode= (mode_t) va_arg(ap, int);
#else
	mode= va_arg(ap, mode_t);
#endif
	va_end(ap);

	/* In pthread environments the dlsym() may call our open(). */
	/* We simply ignore it because libc is already loaded       */
	if (initing) {
		errno = EFAULT;
		return -1;
	}

	if (eatmydata_is_hungry())
		flags &= ~(O_SYNC|O_DSYNC);

	return REAL(open)(pathname,flags,mode);
}

#ifndef __USE_FILE_OFFSET64
int EATMYDATA(open64)(const char* pathname, int flags, ...)
{
	va_list ap;
	mode_t mode;

	va_start(ap, flags);
#if SIZEOF_MODE_T < SIZEOF_INT
	mode= (mode_t) va_arg(ap, int);
#else
	mode= va_arg(ap, mode_t);
#endif
	va_end(ap);

	/* In pthread environments the dlsym() may call our open(). */
	/* We simply ignore it because libc is already loaded       */
	if (initing) {
		errno = EFAULT;
		return -1;
	}

	if (eatmydata_is_hungry())
		flags &= ~(O_SYNC|O_DSYNC);

	return REAL(open64)(pathname,flags,mode);
}
#endif

int EATMYDATA(fdatasync)(int fd)
{
	if (eatmydata_is_hungry()) {
		pthread_testcancel();
		errno= 0;
		return 0;
	}

	return REAL(fdatasync)(fd);
}

int EATMYDATA(msync)(void *addr, size_t length, int flags)
{
	if (eatmydata_is_hungry()) {
		pthread_testcancel();
		errno= 0;
		return 0;
	}

	return REAL(msync)(addr, length, flags);
}

#ifdef HAVE_SYNC_FILE_RANGE
int EATMYDATA(sync_file_range)(int fd, off64_t offset, off64_t nbytes, unsigned int flags)
{
	if (eatmydata_is_hungry()) {
		pthread_testcancel();
		errno= 0;
		return 0;
	}

	return REAL(sync_file_range)(fd, offset, nbytes, flags);
}
#endif
