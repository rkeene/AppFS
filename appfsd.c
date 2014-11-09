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

/*
 * Default cache directory
 */
#ifndef APPFS_CACHEDIR
#define APPFS_CACHEDIR "/var/cache/appfs"
#endif

/* Debugging macros */
#ifdef DEBUG
#define APPFS_DEBUG(x...) { fprintf(stderr, "[debug] %s:%i:%s: ", __FILE__, __LINE__, __func__); fprintf(stderr, x); fprintf(stderr, "\n"); }
#else
#define APPFS_DEBUG(x...) /**/
#endif

/*
 * SHA1 Tcl Package initializer, from sha1.o
 */
int Sha1_Init(Tcl_Interp *interp);

/*
 * Thread Specific Data (TSD) for Tcl Interpreter for the current thread
 */
static pthread_key_t interpKey;

/*
 * Global variables, needed for all threads but only initialized before any
 * FUSE threads are created
 */
const char *appfs_cachedir;
time_t appfs_boottime;
int appfs_fuse_started = 0;

/*
 * AppFS Path Type:  Describes the type of path a given file is
 */
typedef enum {
	APPFS_PATHTYPE_INVALID,
	APPFS_PATHTYPE_FILE,
	APPFS_PATHTYPE_DIRECTORY,
	APPFS_PATHTYPE_SYMLINK
} appfs_pathtype_t;

/*
 * AppFS Path Information:
 *         Completely describes a specific path, how it should be returned to
 *         to the kernel
 */
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
		} file;
		struct {
			off_t size;
			char source[256];
		} symlink;
	} typeinfo;
};

/*
 * Create a new Tcl interpreter and completely initialize it
 */
static Tcl_Interp *appfs_create_TclInterp(char **error_string) {
	Tcl_Interp *interp;
	int tcl_ret;

	APPFS_DEBUG("Creating new Tcl interpreter for TID = 0x%llx", (unsigned long long) pthread_self());

	interp = Tcl_CreateInterp();
	if (interp == NULL) {
		fprintf(stderr, "Unable to create Tcl Interpreter.  Aborting.\n");

		if (error_string) {
			*error_string = strdup("Unable to create Tcl interpreter.");
		}

		return(NULL);
	}

	tcl_ret = Tcl_Init(interp);
	if (tcl_ret != TCL_OK) {
		fprintf(stderr, "Unable to initialize Tcl.  Aborting.\n");
		fprintf(stderr, "Tcl Error is: %s\n", Tcl_GetStringResult(interp));

		if (error_string) {
			*error_string = strdup(Tcl_GetStringResult(interp));
		}

		Tcl_DeleteInterp(interp);

		return(NULL);
	}

	tcl_ret = Tcl_Eval(interp, "package ifneeded sha1 1.0 [list load {} sha1]");
	if (tcl_ret != TCL_OK) {
		fprintf(stderr, "Unable to initialize Tcl SHA1.  Aborting.\n");
		fprintf(stderr, "Tcl Error is: %s\n", Tcl_GetStringResult(interp));

		if (error_string) {
			*error_string = strdup(Tcl_GetStringResult(interp));
		}

		Tcl_DeleteInterp(interp);

		return(NULL);
	}

	tcl_ret = Tcl_Eval(interp, "package ifneeded appfsd 1.0 [list load {} appfsd]");
	if (tcl_ret != TCL_OK) {
		fprintf(stderr, "Unable to initialize Tcl AppFS Package.  Aborting.\n");
		fprintf(stderr, "Tcl Error is: %s\n", Tcl_GetStringResult(interp));

		if (error_string) {
			*error_string = strdup(Tcl_GetStringResult(interp));
		}

		Tcl_DeleteInterp(interp);

		return(NULL);
	}

	/*
	 * Load the "appfsd.tcl" script, which is "compiled" into a C header
	 * so that it does not need to exist on the filesystem and can be
	 * directly evaluated.
	 */
	tcl_ret = Tcl_Eval(interp, ""
#include "appfsd.tcl.h"
	"");
	if (tcl_ret != TCL_OK) {
		fprintf(stderr, "Unable to initialize Tcl AppFS script.  Aborting.\n");
		fprintf(stderr, "Tcl Error is: %s\n", Tcl_GetStringResult(interp));

		if (error_string) {
			*error_string = strdup(Tcl_GetStringResult(interp));
		}

		Tcl_DeleteInterp(interp);

		return(NULL);
	}

	/*
	 * Set global variables from C to Tcl
	 */
	if (Tcl_SetVar(interp, "::appfs::cachedir", appfs_cachedir, TCL_GLOBAL_ONLY) == NULL) {
		fprintf(stderr, "Unable to set cache directory.  This should never fail.\n");

		if (error_string) {
			*error_string = strdup(Tcl_GetStringResult(interp));
		}

		Tcl_DeleteInterp(interp);

		return(NULL);
	}

	/*
	 * Initialize the "appfsd.tcl" environment, which must be done after
	 * global variables are set.
	 */
	tcl_ret = Tcl_Eval(interp, "::appfs::init");
	if (tcl_ret != TCL_OK) {
		fprintf(stderr, "Unable to initialize Tcl AppFS script (::appfs::init).  Aborting.\n");
		fprintf(stderr, "Tcl Error is: %s\n", Tcl_GetStringResult(interp));

		if (error_string) {
			*error_string = strdup(Tcl_GetStringResult(interp));
		}

		Tcl_DeleteInterp(interp);

		return(NULL);
	}

	/*
	 * Hide some Tcl commands that we do not care to use and which may
	 * slow down run-time operations.
	 */
	Tcl_HideCommand(interp, "auto_load_index", "auto_load_index");
	Tcl_HideCommand(interp, "unknown", "unknown");

	/*
	 * Return the completely initialized interpreter
	 */
	return(interp);
}

/*
 * Return the thread-specific Tcl interpreter, creating it if needed
 */
static Tcl_Interp *appfs_TclInterp(void) {
	Tcl_Interp *interp;
	int pthread_ret;

	interp = pthread_getspecific(interpKey);
	if (interp == NULL) {
		interp = appfs_create_TclInterp(NULL);

		if (interp == NULL) {
			return(NULL);
		}

		pthread_ret = pthread_setspecific(interpKey, interp);
		if (pthread_ret != 0) {
			Tcl_DeleteInterp(interp);

			return(NULL);
		}
	}

	return(interp);
}

/*
 * Evaluate a Tcl script constructed by concatenating a bunch of C strings
 * together.
 */
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

/*
 * Determine the UID for the user making the current FUSE filesystem request.
 * This will be used to lookup the user's home directory so we can search for
 * locally modified files.
 */
static uid_t appfs_get_fsuid(void) {
	struct fuse_context *ctx;

	if (!appfs_fuse_started) {
		return(getuid());
	}

	ctx = fuse_get_context();
	if (ctx == NULL) {
		/* Unable to lookup user for some reason */
		/* Return an unprivileged user ID */
		return(1);
	}

	return(ctx->uid);
}

/*
 * Look up the home directory for a given UID
 *        Returns a C string containing the user's home directory or NULL if
 *        the user's home directory does not exist or is not correctly
 *        configured
 */
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

/*
 * Tcl interface to get the home directory for the user making the "current"
 * FUSE I/O request
 */
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

/*
 * Generate an inode for a given path.  The inode should be computed in such
 * a way that it is unlikely to be duplicated and remains the same for a given
 * file
 */
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
static int appfs_get_path_info(const char *path, struct appfs_pathinfo *pathinfo) {
	Tcl_Interp *interp;
	Tcl_Obj *attrs_dict, *attr_value;
	const char *attr_value_str;
	Tcl_WideInt attr_value_wide;
	int attr_value_int;
	static __thread Tcl_Obj *attr_key_type = NULL, *attr_key_perms = NULL, *attr_key_size = NULL, *attr_key_time = NULL, *attr_key_source = NULL, *attr_key_childcount = NULL, *attr_key_packaged = NULL;
	int tcl_ret;

	interp = appfs_TclInterp();
	if (interp == NULL) {
		return(-EIO);
	}

	tcl_ret = appfs_Tcl_Eval(interp, 2, "::appfs::getattr", path);
	if (tcl_ret != TCL_OK) {
		APPFS_DEBUG("::appfs::getattr(%s) failed.", path);
		APPFS_DEBUG("Tcl Error is: %s", Tcl_GetStringResult(interp));

		return(-ENOENT);
	}

	if (attr_key_type == NULL) {
		attr_key_type       = Tcl_NewStringObj("type", -1);
		attr_key_perms      = Tcl_NewStringObj("perms", -1);
		attr_key_size       = Tcl_NewStringObj("size", -1);
		attr_key_time       = Tcl_NewStringObj("time", -1);
		attr_key_source     = Tcl_NewStringObj("source", -1);
		attr_key_childcount = Tcl_NewStringObj("childcount", -1);
		attr_key_packaged   = Tcl_NewStringObj("packaged", -1);
	}

	attrs_dict = Tcl_GetObjResult(interp);
	tcl_ret = Tcl_DictObjGet(interp, attrs_dict, attr_key_type, &attr_value);
	if (tcl_ret != TCL_OK) {
		APPFS_DEBUG("[dict get \"type\"] failed");
		APPFS_DEBUG("Tcl Error is: %s", Tcl_GetStringResult(interp));

		return(-EIO);
	}

	if (attr_value == NULL) {
		return(-EIO);
	}

	pathinfo->packaged = 0;
	pathinfo->inode = appfs_get_path_inode(path);

	attr_value_str = Tcl_GetString(attr_value);
	switch (attr_value_str[0]) {
		case 'd': /* directory */
			pathinfo->type = APPFS_PATHTYPE_DIRECTORY;
			pathinfo->typeinfo.dir.childcount = 0;

			Tcl_DictObjGet(interp, attrs_dict, attr_key_childcount, &attr_value);
			if (attr_value != NULL) {
				tcl_ret = Tcl_GetWideIntFromObj(NULL, attr_value, &attr_value_wide);
				if (tcl_ret == TCL_OK) {
					pathinfo->typeinfo.dir.childcount = attr_value_wide;
				}
			}

			break;
		case 'f': /* file */
			pathinfo->type = APPFS_PATHTYPE_FILE;
			pathinfo->typeinfo.file.size = 0;
			pathinfo->typeinfo.file.executable = 0;

			Tcl_DictObjGet(interp, attrs_dict, attr_key_size, &attr_value);
			if (attr_value != NULL) {
				tcl_ret = Tcl_GetWideIntFromObj(NULL, attr_value, &attr_value_wide);
				if (tcl_ret == TCL_OK) {
					pathinfo->typeinfo.file.size = attr_value_wide;
				}
			}

			Tcl_DictObjGet(interp, attrs_dict, attr_key_perms, &attr_value);
			if (attr_value != NULL) {
				attr_value_str = Tcl_GetString(attr_value);
				if (attr_value_str[0] == 'x') {
					pathinfo->typeinfo.file.executable = 1;
				}
			}
			break;
		case 's': /* symlink */
			pathinfo->type = APPFS_PATHTYPE_SYMLINK;
			pathinfo->typeinfo.symlink.size = 0;
			pathinfo->typeinfo.symlink.source[0] = '\0';

			Tcl_DictObjGet(interp, attrs_dict, attr_key_source, &attr_value);
			if (attr_value != NULL) {
				attr_value_str = Tcl_GetStringFromObj(attr_value, &attr_value_int); 

				if ((attr_value_int + 1) <= sizeof(pathinfo->typeinfo.symlink.source)) {
					pathinfo->typeinfo.symlink.size = attr_value_int;
					pathinfo->typeinfo.symlink.source[attr_value_int] = '\0';

					memcpy(pathinfo->typeinfo.symlink.source, attr_value_str, attr_value_int);
				}
			}
			break;
		default:
			return(-EIO);
	}

	Tcl_DictObjGet(interp, attrs_dict, attr_key_packaged, &attr_value);
	if (attr_value != NULL) {
		pathinfo->packaged = 1;
	}

	Tcl_DictObjGet(interp, attrs_dict, attr_key_time, &attr_value);
	if (attr_value != NULL) {
		tcl_ret = Tcl_GetWideIntFromObj(NULL, attr_value, &attr_value_wide);
		if (tcl_ret == TCL_OK) {
			pathinfo->time = attr_value_wide;
		}
	} else {
		pathinfo->time = 0;
	}

	return(0);
}

static int appfs_fuse_readlink(const char *path, char *buf, size_t size) {
	struct appfs_pathinfo pathinfo;
	int retval = 0;

	APPFS_DEBUG("Enter (path = %s, ...)", path);

	pathinfo.type = APPFS_PATHTYPE_INVALID;

	retval = appfs_get_path_info(path, &pathinfo);
	if (retval != 0) {
		return(retval);
	}

	if (pathinfo.type != APPFS_PATHTYPE_SYMLINK) {
		return(-EINVAL);
	}

	if ((strlen(pathinfo.typeinfo.symlink.source) + 1) > size) {
		return(-ENAMETOOLONG);
	}

	memcpy(buf, pathinfo.typeinfo.symlink.source, strlen(pathinfo.typeinfo.symlink.source) + 1);

	return(0);
}

static int appfs_fuse_getattr(const char *path, struct stat *stbuf) {
	struct appfs_pathinfo pathinfo;
	int retval;

	retval = 0;

	APPFS_DEBUG("Enter (path = %s, ...)", path);

	pathinfo.type = APPFS_PATHTYPE_INVALID;

	retval = appfs_get_path_info(path, &pathinfo);
	if (retval != 0) {
		return(retval);
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
			retval = -EIO;

			break;
	}

	if (pathinfo.packaged) {
		stbuf->st_mode |= 0222;
	}

	return(retval);
}

static int appfs_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
	Tcl_Interp *interp;
	Tcl_Obj **children;
	int children_count, idx;
	int tcl_ret;

	APPFS_DEBUG("Enter (path = %s, ...)", path);

	interp = appfs_TclInterp();
	if (interp == NULL) {
		return(0);
	}

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	tcl_ret = appfs_Tcl_Eval(interp, 2, "::appfs::getchildren", path);
	if (tcl_ret != TCL_OK) {
		APPFS_DEBUG("::appfs::getchildren(%s) failed.", path);
		APPFS_DEBUG("Tcl Error is: %s", Tcl_GetStringResult(interp));

		return(0);
	}

	tcl_ret = Tcl_ListObjGetElements(interp, Tcl_GetObjResult(interp), &children_count, &children);
	if (tcl_ret != TCL_OK) {
		APPFS_DEBUG("Parsing list of children on path %s failed.", path);
		APPFS_DEBUG("Tcl Error is: %s", Tcl_GetStringResult(interp));

		return(0);
	}

	for (idx = 0; idx < children_count; idx++) {
		filler(buf, Tcl_GetString(children[idx]), NULL, 0);
	}

	return(0);
}

static int appfs_fuse_open(const char *path, struct fuse_file_info *fi) {
	Tcl_Interp *interp;
	struct appfs_pathinfo pathinfo;
	const char *real_path, *mode;
	int gpi_ret, tcl_ret;
	int fh;

	APPFS_DEBUG("Enter (path = %s, ...)", path);

	gpi_ret = appfs_get_path_info(path, &pathinfo);

	if ((fi->flags & (O_WRONLY|O_CREAT)) == (O_CREAT|O_WRONLY)) {
		/* The file will be created if it does not exist */
		if (gpi_ret != 0 && gpi_ret != -ENOENT) {
			return(gpi_ret);
		}

		mode = "create";
	} else {
		/* The file must already exist */
		if (gpi_ret != 0) {
			return(gpi_ret);
		}

		mode = "";

		if ((fi->flags & O_WRONLY) == O_WRONLY) {
			mode = "write";
		}
	}

	if (pathinfo.type == APPFS_PATHTYPE_DIRECTORY) {
		return(-EISDIR);
	}

	interp = appfs_TclInterp();
	if (interp == NULL) {
		return(-EIO);
	}

	tcl_ret = appfs_Tcl_Eval(interp, 3, "::appfs::openpath", path, mode);
	if (tcl_ret != TCL_OK) {
		APPFS_DEBUG("::appfs::openpath(%s, %s) failed.", path, mode);
		APPFS_DEBUG("Tcl Error is: %s", Tcl_GetStringResult(interp));

		return(-EIO);
	}

	real_path = Tcl_GetStringResult(interp);
	if (real_path == NULL) {
		return(-EIO);
	}

	APPFS_DEBUG("Translated request to open %s to opening %s (mode = \"%s\")", path, real_path, mode);

	fh = open(real_path, fi->flags, 0600);
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

static int appfs_fuse_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	off_t lseek_ret;
	ssize_t write_ret;

	APPFS_DEBUG("Enter (path = %s, ...)", path);

	lseek_ret = lseek(fi->fh, offset, SEEK_SET);
	if (lseek_ret != offset) {
		return(-EIO);
	}

	write_ret = write(fi->fh, buf, size);

	return(write_ret);
}

/*
 * SQLite3 mode: Execute raw SQL and return success or failure
 */
static int appfs_sqlite3(const char *sql) {
	Tcl_Interp *interp;
	const char *sql_ret;
	int tcl_ret;

	interp = appfs_create_TclInterp(NULL);
	if (interp == NULL) {
		fprintf(stderr, "Unable to create a Tcl interpreter.  Aborting.\n");

		return(1);
	}

	tcl_ret = appfs_Tcl_Eval(interp, 5, "::appfs::db", "eval", sql, "row", "unset -nocomplain row(*); parray row; puts \"----\"");
	sql_ret = Tcl_GetStringResult(interp);

	if (tcl_ret != TCL_OK) {
		fprintf(stderr, "[error] %s\n", sql_ret);

		return(1);
	}

	if (sql_ret && sql_ret[0] != '\0') {
		printf("%s\n", sql_ret);
	}

	return(0);
}

/*
 * Tcl mode: Execute raw Tcl and return success or failure
 */
static int appfs_tcl(const char *tcl) {
	Tcl_Interp *interp;
	const char *tcl_result;
	int tcl_ret;

	interp = appfs_create_TclInterp(NULL);
	if (interp == NULL) {
		fprintf(stderr, "Unable to create a Tcl interpreter.  Aborting.\n");

		return(1);
	}

	tcl_ret = Tcl_Eval(interp, tcl);
	tcl_result = Tcl_GetStringResult(interp);

	if (tcl_ret != TCL_OK) {
		fprintf(stderr, "[error] %s\n", tcl_result);

		return(1);
	}

	if (tcl_result && tcl_result[0] != '\0') {
		printf("%s\n", tcl_result);
	}

	return(0);
}

/*
 * AppFSd Package for Tcl:
 *         Bridge for I/O operations to request information about the current
 *         transaction
 */
static int Appfsd_Init(Tcl_Interp *interp) {
#ifdef USE_TCL_STUBS
	if (Tcl_InitStubs(interp, TCL_VERSION, 0) == 0L) {
		return(TCL_ERROR);
	}
#endif

	Tcl_CreateObjCommand(interp, "appfsd::get_homedir", tcl_appfs_get_homedir, NULL, NULL);

	Tcl_PkgProvide(interp, "appfsd", "1.0");

	return(TCL_OK);
}

/*
 * FUSE operations structure
 */
static struct fuse_operations appfs_operations = {
	.getattr   = appfs_fuse_getattr,
	.readdir   = appfs_fuse_readdir,
	.readlink  = appfs_fuse_readlink,
	.open      = appfs_fuse_open,
	.release   = appfs_fuse_close,
	.read      = appfs_fuse_read,
	.write     = appfs_fuse_write
};

/*
 * FUSE option parsing callback
 */
static int appfs_fuse_opt_cb(void *data, const char *arg, int key, struct fuse_args *outargs) {
	static int seen_cachedir = 0;

	if (key == FUSE_OPT_KEY_NONOPT && seen_cachedir == 0) {
		seen_cachedir = 1;

		appfs_cachedir = strdup(arg);

		return(0);
	}

	return(1);
}

/*
 * Entry point into this program.
 */
int main(int argc, char **argv) {
	Tcl_Interp *test_interp;
	char *test_interp_error;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	int pthread_ret;

	/*
	 * Skip passed program name
	 */
	if (argc == 0 || argv == NULL) {
		return(1);
	}
	argc--;
	argv++;

	/*
	 * Set global variables, these should be configuration options.
	 */
	appfs_cachedir = APPFS_CACHEDIR;

	/*
	 * Set global variable for "boot time" to set a time on directories
	 * that we fake.
	 */
	appfs_boottime = time(NULL);

	/*
	 * Register "sha1" and "appfsd" package with libtcl so that any new
	 * interpreters created (which are done dynamically by FUSE) can have
	 * the appropriate configuration done automatically.
	 */
	Tcl_StaticPackage(NULL, "sha1", Sha1_Init, NULL);
	Tcl_StaticPackage(NULL, "appfsd", Appfsd_Init, NULL);

	/*
	 * Create a thread-specific-data (TSD) key for each thread to refer
	 * to its own Tcl interpreter.  Tcl interpreters must be unique per
	 * thread and new threads are dynamically created by FUSE.
	 */
	pthread_ret = pthread_key_create(&interpKey, NULL);
	if (pthread_ret != 0) {
		fprintf(stderr, "Unable to create TSD key for Tcl.  Aborting.\n");

		return(1);
	}

	/*
	 * Manually specify cache directory, without FUSE callback
	 * This option only works when not using FUSE, since we
	 * do not process it with FUSEs option processing.
	 */
	if (argc >= 2) {
		if (strcmp(argv[0], "--cachedir") == 0) {
			appfs_cachedir = strdup(argv[1]);

			argc -= 2;
			argv += 2;
		}
	}

	/*
	 * SQLite3 mode, for running raw SQL against the cache database
	 */
	if (argc == 2 && strcmp(argv[0], "--sqlite3") == 0) {
		return(appfs_sqlite3(argv[1]));
	}

	/*
	 * Tcl mode, for running raw Tcl in the same environment AppFSd would
	 * run code.
	 */
	if (argc == 2 && strcmp(argv[0], "--tcl") == 0) {
		return(appfs_tcl(argv[1]));
	}

	/*
	 * Add FUSE arguments which we always supply
	 */
	fuse_opt_parse(&args, NULL, NULL, appfs_fuse_opt_cb);
	fuse_opt_add_arg(&args, "-odefault_permissions,fsname=appfs,subtype=appfsd,use_ino,kernel_cache,entry_timeout=60,attr_timeout=3600,intr,big_writes");

	if (getuid() == 0) {
		fuse_opt_parse(&args, NULL, NULL, NULL);
		fuse_opt_add_arg(&args, "-oallow_other");
	}

	/*
	 * Create a Tcl interpreter just to verify that things are in working 
	 * order before we become a daemon.
	 */
	test_interp = appfs_create_TclInterp(&test_interp_error);
	if (test_interp == NULL) {
		if (test_interp_error == NULL) {
			test_interp_error = "Unknown error";
		}

		fprintf(stderr, "Unable to initialize Tcl interpreter for AppFSd:\n");
		fprintf(stderr, "%s", test_interp_error);

		return(1);
	}
	Tcl_DeleteInterp(test_interp);

	/*
	 * Enter the FUSE main loop -- this will process any arguments
	 * and start servicing requests.
	 */
	appfs_fuse_started = 1;
	return(fuse_main(args.argc, args.argv, &appfs_operations, NULL));
}
 
