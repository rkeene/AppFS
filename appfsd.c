#define FUSE_USE_VERSION 26

#include <sys/types.h>
#include <pthread.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <fuse.h>
#include <pwd.h>
#include <tcl.h>

/* From sha1.c */
int Sha1_Init(Tcl_Interp *interp);

#ifndef APPFS_CACHEDIR
#define APPFS_CACHEDIR "/var/cache/appfs"
#endif

#ifdef DEBUG
#define APPFS_DEBUG(x...) { fprintf(stderr, "[debug] %s:%i:%s: ", __FILE__, __LINE__, __func__); fprintf(stderr, x); fprintf(stderr, "\n"); }
#else
#define APPFS_DEBUG(x...) /**/
#endif

static pthread_key_t interpKey;

struct appfs_thread_data {
	const char *cachedir;
	time_t boottime;
	struct {
		int writable;
	} options;
};

struct appfs_thread_data globalThread;

typedef enum {
	APPFS_PATHTYPE_INVALID,
	APPFS_PATHTYPE_FILE,
	APPFS_PATHTYPE_DIRECTORY,
	APPFS_PATHTYPE_SYMLINK
} appfs_pathtype_t;

struct appfs_children {
	struct appfs_children *_next;
	int counter;

	char name[256];
};

struct appfs_pathinfo {
	appfs_pathtype_t type;
	time_t time;
	char hostname[256];
	int packaged;
	unsigned long long inode;
	union {
		struct {
			int childcount;
		} dir;
		struct {
			int executable;
			off_t size;
			char sha1[41];
		} file;
		struct {
			off_t size;
			char source[256];
		} symlink;
	} typeinfo;
};

static Tcl_Interp *appfs_create_TclInterp(const char *cachedir) {
	Tcl_Interp *interp;
	int tcl_ret;

	APPFS_DEBUG("Creating new Tcl interpreter for TID = 0x%llx", (unsigned long long) pthread_self());

	interp = Tcl_CreateInterp();
	if (interp == NULL) {
		fprintf(stderr, "Unable to create Tcl Interpreter.  Aborting.\n");

		return(NULL);
	}

	tcl_ret = Tcl_Init(interp);
	if (tcl_ret != TCL_OK) {
		fprintf(stderr, "Unable to initialize Tcl.  Aborting.\n");
		fprintf(stderr, "Tcl Error is: %s\n", Tcl_GetStringResult(interp));

		Tcl_DeleteInterp(interp);

		return(NULL);
	}

	tcl_ret = Tcl_Eval(interp, "package ifneeded sha1 1.0 [list load {} sha1]");
	if (tcl_ret != TCL_OK) {
		fprintf(stderr, "Unable to initialize Tcl SHA1.  Aborting.\n");
		fprintf(stderr, "Tcl Error is: %s\n", Tcl_GetStringResult(interp));

		Tcl_DeleteInterp(interp);

		return(NULL);
	}

	tcl_ret = Tcl_Eval(interp, "package ifneeded appfsd 1.0 [list load {} appfsd]");
	if (tcl_ret != TCL_OK) {
		fprintf(stderr, "Unable to initialize Tcl AppFS Package.  Aborting.\n");
		fprintf(stderr, "Tcl Error is: %s\n", Tcl_GetStringResult(interp));

		Tcl_DeleteInterp(interp);

		return(NULL);
	}

	tcl_ret = Tcl_Eval(interp, ""
#include "appfsd.tcl.h"
	"");
	if (tcl_ret != TCL_OK) {
		fprintf(stderr, "Unable to initialize Tcl AppFS script.  Aborting.\n");
		fprintf(stderr, "Tcl Error is: %s\n", Tcl_GetStringResult(interp));

		Tcl_DeleteInterp(interp);

		return(NULL);
	}

	if (Tcl_SetVar(interp, "::appfs::cachedir", cachedir, TCL_GLOBAL_ONLY) == NULL) {
		fprintf(stderr, "Unable to set cache directory.  This should never fail.\n");

		Tcl_DeleteInterp(interp);

		return(NULL);
	}

	tcl_ret = Tcl_Eval(interp, "::appfs::init");
	if (tcl_ret != TCL_OK) {
		fprintf(stderr, "Unable to initialize Tcl AppFS script (::appfs::init).  Aborting.\n");
		fprintf(stderr, "Tcl Error is: %s\n", Tcl_GetStringResult(interp));

		Tcl_DeleteInterp(interp);

		return(NULL);
	}

	Tcl_HideCommand(interp, "glob", "glob");
	Tcl_HideCommand(interp, "exec", "exec");
	Tcl_HideCommand(interp, "pid", "pid");
	Tcl_HideCommand(interp, "auto_load_index", "auto_load_index");
	Tcl_HideCommand(interp, "unknown", "unknown");

	return(interp);
}

static int appfs_Tcl_Eval(Tcl_Interp *interp, int objc, const char *cmd, ...) {
	Tcl_Obj **objv;
	const char *arg;
	va_list argp;
	int retval;
	int i;

	if (interp == NULL) {
		return(TCL_ERROR);
	}

	objv = (void *) ckalloc(sizeof(*objv) * objc);
	objv[0] = Tcl_NewStringObj(cmd, -1);
	Tcl_IncrRefCount(objv[0]);

	va_start(argp, cmd);
	for (i = 1; i < objc; i++) {
		arg = va_arg(argp, const char *);
		objv[i] = Tcl_NewStringObj(arg, -1);
		Tcl_IncrRefCount(objv[i]);
	}
	va_end(argp);

	retval = Tcl_EvalObjv(interp, objc, objv, 0);

	for (i = 0; i < objc; i++) {
		Tcl_DecrRefCount(objv[i]);
	}

	ckfree((void *) objv);

	if (retval != TCL_OK) {
		APPFS_DEBUG("Tcl command failed, ::errorInfo contains: %s\n", Tcl_GetVar(interp, "::errorInfo", 0));
	}

	return(retval);
}

static void appfs_update_index(const char *hostname) {
	Tcl_Interp *interp;
	int tcl_ret;

	APPFS_DEBUG("Enter: hostname = %s", hostname);

	interp = pthread_getspecific(interpKey);
	if (interp == NULL) {
		interp = appfs_create_TclInterp(globalThread.cachedir);

		if (interp == NULL) {
			return;
		}

		pthread_setspecific(interpKey, interp);
	}

	tcl_ret = appfs_Tcl_Eval(interp, 2, "::appfs::getindex", hostname);
	if (tcl_ret != TCL_OK) {
		APPFS_DEBUG("Call to ::appfs::getindex failed: %s", Tcl_GetStringResult(interp));

		return;
	}

	return;
}

static const char *appfs_getfile(const char *hostname, const char *sha1) {
	Tcl_Interp *interp;
	char *retval;
	int tcl_ret;

	interp = pthread_getspecific(interpKey);
	if (interp == NULL) {
		interp = appfs_create_TclInterp(globalThread.cachedir);

		if (interp == NULL) {
			return(NULL);
		}

		pthread_setspecific(interpKey, interp);
	}

	tcl_ret = appfs_Tcl_Eval(interp, 3, "::appfs::download", hostname, sha1);
	if (tcl_ret != TCL_OK) {
		APPFS_DEBUG("Call to ::appfs::download failed: %s", Tcl_GetStringResult(interp));

		return(NULL);
	}

	retval = strdup(Tcl_GetStringResult(interp));

	return(retval);
}

static void appfs_update_manifest(const char *hostname, const char *sha1) {
	Tcl_Interp *interp;
	int tcl_ret;

	interp = pthread_getspecific(interpKey);
	if (interp == NULL) {
		interp = appfs_create_TclInterp(globalThread.cachedir);

		if (interp == NULL) {
			return;
		}

		pthread_setspecific(interpKey, interp);
	}

	tcl_ret = appfs_Tcl_Eval(interp, 3, "::appfs::getpkgmanifest", hostname, sha1);
	if (tcl_ret != TCL_OK) {
		APPFS_DEBUG("Call to ::appfs::getpkgmanifest failed: %s", Tcl_GetStringResult(interp));

		return;
	}

	return;
}

static uid_t appfs_get_fsuid(void) {
	struct fuse_context *ctx;

	ctx = fuse_get_context();
	if (ctx == NULL) {
		return(1);
	}

	return(ctx->uid);
}

static char *appfs_get_homedir(uid_t fsuid) {
	struct passwd entry, *result;
	struct stat stbuf;
	char buf[1024], *retval;
	int gpu_ret, stat_ret;

	gpu_ret = getpwuid_r(fsuid, &entry, buf, sizeof(buf), &result);
	if (gpu_ret != 0) {
		APPFS_DEBUG("getpwuid_r(%llu, ...) returned in failure", (unsigned long long) fsuid);

		return(NULL);
	}

	if (result == NULL) {
		APPFS_DEBUG("getpwuid_r(%llu, ...) returned NULL result", (unsigned long long) fsuid);

		return(NULL);
	}

	if (result->pw_dir == NULL) {
		APPFS_DEBUG("getpwuid_r(%llu, ...) returned NULL home directory", (unsigned long long) fsuid);

		return(NULL);
	}

	stat_ret = stat(result->pw_dir, &stbuf);
	if (stat_ret != 0) {
		APPFS_DEBUG("stat(%s) returned in failure", result->pw_dir);

		return(NULL);
	}

	if (stbuf.st_uid != fsuid) {
		APPFS_DEBUG("UID mis-match on user %llu's home directory (%s).  It's owned by %llu.",
		    (unsigned long long) fsuid,
		    result->pw_dir,
		    (unsigned long long) stbuf.st_uid
		);

		return(NULL);
	}

	retval = strdup(result->pw_dir);

	return(retval);
}

static int tcl_appfs_get_homedir(ClientData cd, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]) {
	char *homedir;

        if (objc != 1) {
                Tcl_WrongNumArgs(interp, 1, objv, NULL);
                return(TCL_ERROR);
        }

	homedir = appfs_get_homedir(appfs_get_fsuid());

	if (homedir == NULL) {
		return(TCL_ERROR);
	}

        Tcl_SetObjResult(interp, Tcl_NewStringObj(homedir, -1));

	free(homedir);

        return(TCL_OK);
}

static struct appfs_children *appfs_getchildren(const char *hostname, const char *package_hash, const char *path, int *children_count_p) {
}

static char *appfs_lookup_package_hash(const char *hostname, const char *package, const char *os, const char *cpuArch, const char *version) {
}

static int appfs_getfileinfo(const char *hostname, const char *package_hash, const char *_path, struct appfs_pathinfo *pathinfo) {
}

/* Generate an inode for a given path */
static long long appfs_get_path_inode(const char *path) {
	long long retval;
	const char *p;

	retval = 10;

	for (p = path; *p; p++) {
		retval %= 4290960290ULL;
		retval += *p;
		retval <<= 7;
	}

	retval += 10;
	retval %= 4294967296ULL;

	return(retval);
}

/* Get information about a path, and optionally list children */
static int appfs_get_path_info(const char *_path, struct appfs_pathinfo *pathinfo, struct appfs_children **children) {
}

static int appfs_fuse_getattr(const char *path, struct stat *stbuf) {
	struct appfs_pathinfo pathinfo;
	int res = 0;

	APPFS_DEBUG("Enter (path = %s, ...)", path);

	pathinfo.type = APPFS_PATHTYPE_INVALID;

	res = appfs_get_path_info(path, &pathinfo, NULL);
	if (res != 0) {
		return(res);
	}

	memset(stbuf, 0, sizeof(struct stat));

	stbuf->st_mtime = pathinfo.time;
	stbuf->st_ctime = pathinfo.time;
	stbuf->st_atime = pathinfo.time;
	stbuf->st_ino   = pathinfo.inode;
	stbuf->st_mode  = 0;

	switch (pathinfo.type) {
		case APPFS_PATHTYPE_DIRECTORY:
			stbuf->st_mode = S_IFDIR | 0555;
			stbuf->st_nlink = 2 + pathinfo.typeinfo.dir.childcount;
			break;
		case APPFS_PATHTYPE_FILE:
			if (pathinfo.typeinfo.file.executable) {
				stbuf->st_mode = S_IFREG | 0555;
			} else {
				stbuf->st_mode = S_IFREG | 0444;
			}

			stbuf->st_nlink = 1;
			stbuf->st_size = pathinfo.typeinfo.file.size;
			break;
		case APPFS_PATHTYPE_SYMLINK:
			stbuf->st_mode = S_IFLNK | 0555;
			stbuf->st_nlink = 1;
			stbuf->st_size = pathinfo.typeinfo.symlink.size;
			break;
		case APPFS_PATHTYPE_INVALID:
			res = -EIO;

			break;
	}

	if (pathinfo.packaged) {
		if (globalThread.options.writable) {
			stbuf->st_mode |= 0222;
		}
	}

	return res;
}

static int appfs_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
	struct appfs_pathinfo pathinfo;
	struct appfs_children *children, *child;
	int retval;

	APPFS_DEBUG("Enter (path = %s, ...)", path);

	retval = appfs_get_path_info(path, &pathinfo, &children);
	if (retval != 0) {
		return(retval);
	}

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	for (child = children; child; child = child->_next) {
		filler(buf, child->name, NULL, 0);
	}

	appfs_free_list_children(children);

	return(0);
}

static int appfs_fuse_open(const char *path, struct fuse_file_info *fi) {
	struct appfs_pathinfo pathinfo;
	const char *real_path;
	int fh;
	int gpi_ret;

	APPFS_DEBUG("Enter (path = %s, ...)", path);

	if ((fi->flags & 3) != O_RDONLY) {
                return(-EACCES);
	}

	gpi_ret = appfs_get_path_info(path, &pathinfo, NULL);
	if (gpi_ret != 0) {
		return(gpi_ret);
	}

	if (pathinfo.type == APPFS_PATHTYPE_DIRECTORY) {
		return(-EISDIR);
	}

	real_path = appfs_getfile(pathinfo.hostname, pathinfo.typeinfo.file.sha1);
	if (real_path == NULL) {
		return(-EIO);
	}

	fh = open(real_path, O_RDONLY);
	free((void *) real_path);
	if (fh < 0) {
		return(-EIO);
	}

	fi->fh = fh;

	return(0);
}

static int appfs_fuse_close(const char *path, struct fuse_file_info *fi) {
	int close_ret;

	close_ret = close(fi->fh);
	if (close_ret != 0) {
		return(-EIO);
	}

	return(0);
}

static int appfs_fuse_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	off_t lseek_ret;
	ssize_t read_ret;

	APPFS_DEBUG("Enter (path = %s, ...)", path);

	lseek_ret = lseek(fi->fh, offset, SEEK_SET);
	if (lseek_ret != offset) {
		return(-EIO);
	}

	read_ret = read(fi->fh, buf, size);

	return(read_ret);
}

static struct fuse_operations appfs_oper = {
	.getattr   = appfs_fuse_getattr,
	.readdir   = appfs_fuse_readdir,
	.readlink  = appfs_fuse_readlink,
	.open      = appfs_fuse_open,
	.release   = appfs_fuse_close,
	.read      = appfs_fuse_read
};

int Appfsd_Init(Tcl_Interp *interp) {
#ifdef USE_TCL_STUBS
	if (Tcl_InitStubs(interp, TCL_VERSION, 0) == 0L) {
		return(TCL_ERROR);
	}
#endif

	Tcl_CreateObjCommand(interp, "appfsd::get_homedir", tcl_appfs_get_homedir, NULL, NULL);

	Tcl_PkgProvide(interp, "appfsd", "1.0");

	return(TCL_OK);
}

int main(int argc, char **argv) {
	const char *cachedir = APPFS_CACHEDIR;
	char dbfilename[1024];
	int pthread_ret, snprintf_ret;

	globalThread.cachedir = cachedir;
	globalThread.boottime = time(NULL);
	globalThread.options.writable = 1;

	Tcl_StaticPackage(NULL, "sha1", Sha1_Init, NULL);
	Tcl_StaticPackage(NULL, "appfsd", Appfsd_Init, NULL);

	pthread_ret = pthread_key_create(&interpKey, NULL);
	if (pthread_ret != 0) {
		fprintf(stderr, "Unable to create TSD key for Tcl.  Aborting.\n");

		return(1);
	}

	snprintf_ret = snprintf(dbfilename, sizeof(dbfilename), "%s/%s", cachedir, "cache.db");
	if (snprintf_ret >= sizeof(dbfilename)) {
		fprintf(stderr, "Unable to set database filename.  Aborting.\n");

		return(1);
	}

	return(fuse_main(argc, argv, &appfs_oper, NULL));
}
 
