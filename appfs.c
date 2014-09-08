#define FUSE_USE_VERSION 26

#include <sqlite3.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <fuse.h>
#include <tcl.h>

#define APPFS_CACHEDIR "/tmp/appfs-cache"

#define APPFS_DEBUG(x...) { fprintf(stderr, "%i:%s: ", __LINE__, __func__); fprintf(stderr, x); fprintf(stderr, "\n"); }

struct appfs_thread_data {
	Tcl_Interp *interp;
	sqlite3 *db;
};

struct appfs_thread_data globalThread;

typedef enum {
	APPFS_OS_UNKNOWN,
	APPFS_OS_ALL,
	APPFS_OS_LINUX,
	APPFS_OS_MACOSX,
	APPFS_OS_FREEBSD,
	APPFS_OS_OPENBSD,
	APPFS_OS_SOLARIS
} appfs_os_t;

typedef enum {
	APPFS_CPU_UNKNOWN,
	APPFS_CPU_ALL,
	APPFS_CPU_AMD64,
	APPFS_CPU_I386,
	APPFS_CPU_ARM
} appfs_cpuArch_t;

struct appfs_package {
	char name[128];
	char version[64];
	char sha1[41];
	appfs_os_t os;
	appfs_cpuArch_t cpuArch;
	int isLatest;

	int counter;
	struct appfs_package *_next;
};

static appfs_os_t appfs_convert_os_fromString(const char *os) {
	if (strcasecmp(os, "Linux") == 0) {
		return(APPFS_OS_LINUX);
	}

	if (strcasecmp(os, "Darwin") == 0 || strcasecmp(os, "Mac OS") == 0 || strcasecmp(os, "Mac OS X") == 0) {
		return(APPFS_OS_MACOSX);
	}

	if (strcasecmp(os, "noarch") == 0) {
		return(APPFS_OS_ALL);
	}

	return(APPFS_OS_UNKNOWN);
}

static const char *appfs_convert_os_toString(appfs_os_t os) {
	switch (os) {
		case APPFS_OS_ALL:
			return("noarch");
		case APPFS_OS_LINUX:
			return("linux");
		case APPFS_OS_MACOSX:
			return("macosx");
		case APPFS_OS_FREEBSD:
			return("freebsd");
		case APPFS_OS_OPENBSD:
			return("openbsd");
		case APPFS_OS_SOLARIS:
			return("freebsd");
		case APPFS_CPU_UNKNOWN:
			return("unknown");
	}

	return("unknown");
}

static appfs_cpuArch_t appfs_convert_cpuArch_fromString(const char *cpu) {
	if (strcasecmp(cpu, "amd64") == 0 || strcasecmp(cpu, "x86_64") == 0) {
		return(APPFS_CPU_AMD64);
	}

	if (strcasecmp(cpu, "i386") == 0 || \
	    strcasecmp(cpu, "i486") == 0 || \
	    strcasecmp(cpu, "i586") == 0 || \
	    strcasecmp(cpu, "i686") == 0 || \
	    strcasecmp(cpu, "ix86") == 0) {
		return(APPFS_CPU_I386);
	}

	if (strcasecmp(cpu, "arm") == 0) {
		return(APPFS_CPU_ARM);
	}

	if (strcasecmp(cpu, "noarch") == 0) {
		return(APPFS_CPU_ALL);
	}

	return(APPFS_CPU_UNKNOWN);
}

static const char *appfs_convert_cpuArch_toString(appfs_cpuArch_t cpu) {
	switch (cpu) {
		case APPFS_CPU_ALL:
			return("noarch");
		case APPFS_CPU_AMD64:
			return("amd64");
		case APPFS_CPU_I386:
			return("ix86");
		case APPFS_CPU_ARM:
			return("arm");
		case APPFS_CPU_UNKNOWN:
			return("unknown");
	}

	return("unknown");
}

static int appfs_Tcl_Eval(Tcl_Interp *interp, int objc, const char *cmd, ...) {
	Tcl_Obj **objv;
	const char *arg;
	va_list argp;
	int retval;
	int i;

	objv = (void *) ckalloc(sizeof(*objv) * objc);
	objv[0] = Tcl_NewStringObj(cmd, -1);

	va_start(argp, cmd);
	for (i = 1; i < objc; i++) {
		arg = va_arg(argp, const char *);
		objv[i] = Tcl_NewStringObj(arg, -1);
	}
	va_end(argp);

	retval = Tcl_EvalObjv(interp, objc, objv, 0);

	ckfree((void *) objv);

	return(retval);
}

int appfs_getindex_cb(void *_head, int columns, char **values, char **names) {
	struct appfs_package **head = _head, *obj;

	obj = (void *) ckalloc(sizeof(*obj));

	snprintf(obj->name, sizeof(obj->name), "%s", values[0]);
	snprintf(obj->version, sizeof(obj->version), "%s", values[1]);
	snprintf(obj->sha1, sizeof(obj->sha1), "%s", values[2]);
	obj->os = appfs_convert_os_fromString(values[3]);
	obj->cpuArch = appfs_convert_cpuArch_fromString(values[4]);
	if (values[5][0] == '1') {
		obj->isLatest = 1;
	} else {
		obj->isLatest = 0;
	}

	if (*head == NULL) {
		obj->counter = 0;
	} else {
		obj->counter = (*head)->counter + 1;
	}

	obj->_next = *head;
	*head = obj;

	return(0);
}

static struct appfs_package *appfs_getindex(const char *hostname, int *package_count_p) {
	struct appfs_package *head = NULL;
	char *sql;
	int tcl_ret, sqlite_ret;

	if (package_count_p == NULL) {
		return(NULL);
	}

	tcl_ret = appfs_Tcl_Eval(globalThread.interp, 2, "::appfs::getindex", hostname);
	if (tcl_ret != TCL_OK) {
		APPFS_DEBUG("Call to ::appfs::getindex failed: %s", Tcl_GetStringResult(globalThread.interp));

		return(NULL);
	}

	sql = sqlite3_mprintf("SELECT package, version, sha1, os, cpuArch, isLatest FROM packages WHERE hostname = %Q;", hostname);
	if (sql == NULL) {
		APPFS_DEBUG("Call to sqlite3_mprintf failed.");

		return(NULL);
	}

	sqlite_ret = sqlite3_exec(globalThread.db, sql, appfs_getindex_cb, &head, NULL);
	sqlite3_free(sql);

	if (sqlite_ret != SQLITE_OK) {
		APPFS_DEBUG("Call to sqlite3_exec failed.");

		return(NULL);
	}

	if (head != NULL) {
		*package_count_p = head->counter + 1;
	}

	return(head);
}

static int appfs_getfile(const char *hostname, const char *sha1) {
	return(0);
}

static int appfs_getmanifest(const char *hostname, const char *sha1) {
	return(0);
}

static int appfs_fuse_getattr(const char *path, struct stat *stbuf) {
	char *hostname;
	int res = 0;

	if (path == NULL) {
		return(-ENOENT);
	}

	APPFS_DEBUG("Enter (path = %s, ...)", path);

	if (path[0] != '/') {
		return(-ENOENT);
	}

	memset(stbuf, 0, sizeof(struct stat));

	stbuf->st_mode = S_IFDIR | 0755;
	stbuf->st_nlink = 2;

	return res;
}

static int appfs_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
	APPFS_DEBUG("Enter (path = %s, ...)", path);

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	return 0;
}

static int appfs_fuse_open(const char *path, struct fuse_file_info *fi) {
	return(-ENOENT);
}

static int appfs_fuse_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	return(-ENOENT);
}

#ifdef APPFS_TEST_DRIVER
static int appfs_test_driver(void) {
	struct appfs_package *packages, *package;
	int packages_count = 0;

	packages = appfs_getindex("rkeene.org", &packages_count);
	if (packages == NULL || packages_count == 0) {
		fprintf(stderr, "Unable to fetch package index from rkeene.org.\n");

		return(1);
	}

	for (package = packages; package; package = package->_next) {
		printf("Package:\n\tname = %s\n\tversion = %s\n\tsha1 = %s\n\tos = %s\n\tcpuArch = %s\n",
			package->name,
			package->version,
			package->sha1,
			appfs_convert_os_toString(package->os),
			appfs_convert_cpuArch_toString(package->cpuArch)
		);
	}

	return(0);
}
#else
static struct fuse_operations appfs_oper = {
	.getattr	= appfs_fuse_getattr,
	.readdir	= appfs_fuse_readdir,
	.open		= appfs_fuse_open,
	.read		= appfs_fuse_read
};
#endif

int main(int argc, char **argv) {
	const char *cachedir = APPFS_CACHEDIR;
	char dbfilename[1024];
	int tcl_ret, snprintf_ret, sqlite_ret;

	globalThread.interp = Tcl_CreateInterp();
	if (globalThread.interp == NULL) {
		fprintf(stderr, "Unable to create Tcl Interpreter.  Aborting.\n");

		return(1);
	}

	tcl_ret = Tcl_Init(globalThread.interp);
	if (tcl_ret != TCL_OK) {
		fprintf(stderr, "Unable to initialize Tcl.  Aborting.\n");

		return(1);
	}

	tcl_ret = Tcl_Eval(globalThread.interp, ""
#include "appfs.tcl.h"
	"");
	if (tcl_ret != TCL_OK) {
		fprintf(stderr, "Unable to initialize Tcl AppFS script.  Aborting.\n");
		fprintf(stderr, "Tcl Error is: %s\n", Tcl_GetStringResult(globalThread.interp));

		return(1);
	}

	if (Tcl_SetVar(globalThread.interp, "::appfs::cachedir", cachedir, TCL_GLOBAL_ONLY) == NULL) {
		fprintf(stderr, "Unable to set cache directory.  This should never fail.\n");

		return(1);
	}

	tcl_ret = appfs_Tcl_Eval(globalThread.interp, 1, "::appfs::init");
	if (tcl_ret != TCL_OK) {
		fprintf(stderr, "Unable to initialize Tcl AppFS script (::appfs::init).  Aborting.\n");
		fprintf(stderr, "Tcl Error is: %s\n", Tcl_GetStringResult(globalThread.interp));

		return(1);
	}

	snprintf_ret = snprintf(dbfilename, sizeof(dbfilename), "%s/%s", cachedir, "cache.db");
	if (snprintf_ret >= sizeof(dbfilename)) {
		fprintf(stderr, "Unable to set database filename.  Aborting.\n");

		return(1);
	}

	sqlite_ret = sqlite3_open(dbfilename, &globalThread.db);
	if (sqlite_ret != SQLITE_OK) {
		fprintf(stderr, "Unable to open database: %s\n", dbfilename);

		return(1);
	}

#ifdef APPFS_TEST_DRIVER
	return(appfs_test_driver());
#else
	return(fuse_main(argc, argv, &appfs_oper, NULL));
#endif
}
 
