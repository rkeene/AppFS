#define FUSE_USE_VERSION 26

#include <sys/types.h>
#include <sqlite3.h>
#include <pthread.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <fuse.h>
#include <tcl.h>

#define APPFS_CACHEDIR "/tmp/appfs-cache"

#define APPFS_DEBUG(x...) { fprintf(stderr, "[debug] %s:%i:%s: ", __FILE__, __LINE__, __func__); fprintf(stderr, x); fprintf(stderr, "\n"); }

static pthread_key_t interpKey;

struct appfs_thread_data {
	sqlite3 *db;
	const char *cachedir;
	time_t boottime;
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

typedef enum {
	APPFS_PATHTYPE_INVALID,
	APPFS_PATHTYPE_FILE,
	APPFS_PATHTYPE_DIRECTORY,
	APPFS_PATHTYPE_SYMLINK
} appfs_pathtype_t;

struct appfs_package {
	struct appfs_package *_next;
	int counter;

	char name[256];
	char version[64];
	char sha1[41];
	char os_str[64];
	char cpuArch_str[64];
	appfs_os_t os;
	appfs_cpuArch_t cpuArch;
	int isLatest;
};

struct appfs_site {
	struct appfs_site *_next;
	int counter;

	char name[256];
};

struct appfs_children {
	struct appfs_children *_next;
	int counter;

	char name[256];
};

struct appfs_pathinfo {
	appfs_pathtype_t type;
	time_t time;
	char hostname[256];
	union {
		struct {
			int childcount;
		} dir;
		struct {
			int executable;
			off_t size;
			char sha1[41];
		} file;
	} typeinfo;
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

static Tcl_Interp *appfs_create_TclInterp(const char *cachedir) {
	Tcl_Interp *interp;
	int tcl_ret;

	interp = Tcl_CreateInterp();
	if (interp == NULL) {
		fprintf(stderr, "Unable to create Tcl Interpreter.  Aborting.\n");

		return(NULL);
	}

	tcl_ret = Tcl_Init(interp);
	if (tcl_ret != TCL_OK) {
		fprintf(stderr, "Unable to initialize Tcl.  Aborting.\n");

		return(NULL);
	}

	tcl_ret = Tcl_Eval(interp, ""
#include "appfs.tcl.h"
	"");
	if (tcl_ret != TCL_OK) {
		fprintf(stderr, "Unable to initialize Tcl AppFS script.  Aborting.\n");
		fprintf(stderr, "Tcl Error is: %s\n", Tcl_GetStringResult(interp));

		return(NULL);
	}

	if (Tcl_SetVar(interp, "::appfs::cachedir", cachedir, TCL_GLOBAL_ONLY) == NULL) {
		fprintf(stderr, "Unable to set cache directory.  This should never fail.\n");

		return(NULL);
	}

	tcl_ret = Tcl_Eval(interp, "::appfs::init");
	if (tcl_ret != TCL_OK) {
		fprintf(stderr, "Unable to initialize Tcl AppFS script (::appfs::init).  Aborting.\n");
		fprintf(stderr, "Tcl Error is: %s\n", Tcl_GetStringResult(interp));

		return(NULL);
	}

	return(interp);
}

static int appfs_Tcl_Eval(Tcl_Interp *interp, int objc, const char *cmd, ...) {
	Tcl_Obj **objv;
	const char *arg;
	va_list argp;
	int retval;
	int i;

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

		pthread_setspecific(interpKey, interp);
	}

	tcl_ret = appfs_Tcl_Eval(interp, 3, "::appfs::getpkgmanifest", hostname, sha1);
	if (tcl_ret != TCL_OK) {
		APPFS_DEBUG("Call to ::appfs::getpkgmanifest failed: %s", Tcl_GetStringResult(interp));

		return;
	}

	return;
}


#define appfs_free_list_type(id, type) static void appfs_free_list_ ## id(type *head) { \
	type *obj, *next; \
	for (obj = head; obj; obj = next) { \
		next = obj->_next; \
		ckfree((void *) obj); \
	} \
}

appfs_free_list_type(site, struct appfs_site)
appfs_free_list_type(package, struct appfs_package)
appfs_free_list_type(children, struct appfs_children)

static int appfs_getsites_cb(void *_head, int columns, char **values, char **names) {
	struct appfs_site **head_p, *obj;

	head_p = _head;

	obj = (void *) ckalloc(sizeof(*obj));
	snprintf(obj->name, sizeof(obj->name), "%s", values[0]);

	if (*head_p == NULL) {
		obj->counter = 0;
	} else {
		obj->counter = (*head_p)->counter + 1;
	}

	obj->_next = *head_p;
	*head_p = obj;

	return(0);
}

static struct appfs_site *appfs_getsites(int *site_count_p) {
	struct appfs_site *head = NULL;
	int sqlite_ret;

	if (site_count_p == NULL) {
		return(NULL);
	}

	sqlite_ret = sqlite3_exec(globalThread.db, "SELECT DISTINCT hostname FROM packages;", appfs_getsites_cb, &head, NULL);
	if (sqlite_ret != SQLITE_OK) {
		APPFS_DEBUG("Call to sqlite3_exec failed.");

		return(NULL);
	}

	if (head != NULL) {
		*site_count_p = head->counter + 1;
	}

	return(head);
}

static int appfs_getindex_cb(void *_head, int columns, char **values, char **names) {
	struct appfs_package **head_p, *obj;

	head_p = _head;

	obj = (void *) ckalloc(sizeof(*obj));

	snprintf(obj->name, sizeof(obj->name), "%s", values[0]);
	snprintf(obj->version, sizeof(obj->version), "%s", values[1]);
	snprintf(obj->sha1, sizeof(obj->sha1), "%s", values[2]);
	obj->os = appfs_convert_os_fromString(values[3]);
	obj->cpuArch = appfs_convert_cpuArch_fromString(values[4]);
	snprintf(obj->os_str, sizeof(obj->os_str), "%s", values[3]);
	snprintf(obj->cpuArch_str, sizeof(obj->cpuArch_str), "%s", values[4]);
	if (values[5][0] == '1') {
		obj->isLatest = 1;
	} else {
		obj->isLatest = 0;
	}

	if (*head_p == NULL) {
		obj->counter = 0;
	} else {
		obj->counter = (*head_p)->counter + 1;
	}

	obj->_next = *head_p;
	*head_p = obj;

	return(0);
}

static struct appfs_package *appfs_getindex(const char *hostname, int *package_count_p) {
	struct appfs_package *head = NULL;
	char *sql;
	int sqlite_ret;

	if (package_count_p == NULL) {
		return(NULL);
	}

	appfs_update_index(hostname);

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

static int appfs_getchildren_cb(void *_head, int columns, char **values, char **names) {
	struct appfs_children **head_p, *obj;

	head_p = _head;

	obj = (void *) ckalloc(sizeof(*obj));

	snprintf(obj->name, sizeof(obj->name), "%s", values[0]);

	if (*head_p == NULL) {
		obj->counter = 0;
	} else {
		obj->counter = (*head_p)->counter + 1;
	}

	obj->_next = *head_p;
	*head_p = obj;

	return(0);
	
}

static struct appfs_children *appfs_getchildren(const char *hostname, const char *package_hash, const char *path, int *children_count_p) {
	struct appfs_children *head = NULL;
	char *sql;
	int sqlite_ret;

	if (children_count_p == NULL) {
		return(NULL);
	}

	appfs_update_index(hostname);
	appfs_update_manifest(hostname, package_hash);

	sql = sqlite3_mprintf("SELECT file_name FROM files WHERE package_sha1 = %Q AND file_directory = %Q;", package_hash, path);
	if (sql == NULL) {
		APPFS_DEBUG("Call to sqlite3_mprintf failed.");

		return(NULL);
	}

	APPFS_DEBUG("SQL: %s", sql);
	sqlite_ret = sqlite3_exec(globalThread.db, sql, appfs_getchildren_cb, &head, NULL);
	sqlite3_free(sql);

	if (sqlite_ret != SQLITE_OK) {
		APPFS_DEBUG("Call to sqlite3_exec failed.");

		return(NULL);
	}

	if (head != NULL) {
		*children_count_p = head->counter + 1;
	}

	return(head);
}

static int appfs_lookup_package_hash_cb(void *_retval, int columns, char **values, char **names) {
	char **retval = _retval;

	*retval = strdup(values[0]);

	return(0);
}

static char *appfs_lookup_package_hash(const char *hostname, const char *package, const char *os, const char *cpuArch, const char *version) {
	char *sql;
	char *retval = NULL;
	int sqlite_ret;

	appfs_update_index(hostname);

	sql = sqlite3_mprintf("SELECT sha1 FROM packages WHERE hostname = %Q AND package = %Q AND os = %Q AND cpuArch = %Q AND version = %Q;",
		hostname,
		package,
		os,
		cpuArch,
		version
	);
	if (sql == NULL) {
		APPFS_DEBUG("Call to sqlite3_mprintf failed.");

		return(NULL);
	}

	APPFS_DEBUG("SQL: %s", sql);
	sqlite_ret = sqlite3_exec(globalThread.db, sql, appfs_lookup_package_hash_cb, &retval, NULL);
	sqlite3_free(sql);

	if (sqlite_ret != SQLITE_OK) {
		APPFS_DEBUG("Call to sqlite3_exec failed.");

		return(NULL);
	}

	return(retval);
}

static int appfs_getfileinfo_cb(void *_pathinfo, int columns, char **values, char **names) {
	struct appfs_pathinfo *pathinfo = _pathinfo;
	const char *type, *time, *source, *size, *perms, *sha1;

	type = values[0];
	time = values[1];
	source = values[2];
	size = values[3];
	perms = values[4];
	sha1 = values[5];

	pathinfo->time = strtoull(time, NULL, 10);

	if (strcmp(type, "file") == 0) {
		pathinfo->type = APPFS_PATHTYPE_FILE;

		if (!size) {
			size = "0";
		}

		if (!perms) {
			perms = "";
		}

		if (!sha1) {
			sha1 = "";
		}

		pathinfo->typeinfo.file.size = strtoull(size, NULL, 10);
		snprintf(pathinfo->typeinfo.file.sha1, sizeof(pathinfo->typeinfo.file.sha1), "%s", sha1);

		if (strcmp(perms, "x") == 0) {
			pathinfo->typeinfo.file.executable = 1;
		} else {
			pathinfo->typeinfo.file.executable = 0;
		}

		return(0);
	}

	if (strcmp(type, "directory") == 0) {
		pathinfo->type = APPFS_PATHTYPE_DIRECTORY;
		pathinfo->typeinfo.dir.childcount = 0;

		return(0);
	}

	return(0);

	/* Until this is used, prevent the compiler from complaining */
	source = source;
}

static int appfs_getfileinfo(const char *hostname, const char *package_hash, const char *_path, struct appfs_pathinfo *pathinfo) {
	char *directory, *file, *path;
	char *sql;
	int sqlite_ret;

	if (pathinfo == NULL) {
		return(-EIO);
	}

	appfs_update_index(hostname);
	appfs_update_manifest(hostname, package_hash);

	path = strdup(_path);
	directory = path;
	file = strrchr(path, '/');
	if (file == NULL) {
		file = path;
		directory = "";
	} else {
		*file = '\0';
		file++;
	}

	sql = sqlite3_mprintf("SELECT type, time, source, size, perms, file_sha1 FROM files WHERE package_sha1 = %Q AND file_directory = %Q AND file_name = %Q;", package_hash, directory, file);
	if (sql == NULL) {
		APPFS_DEBUG("Call to sqlite3_mprintf failed.");

		free(path);

		return(-EIO);
	}

	free(path);

	pathinfo->type = APPFS_PATHTYPE_INVALID;

	APPFS_DEBUG("SQL: %s", sql);
	sqlite_ret = sqlite3_exec(globalThread.db, sql, appfs_getfileinfo_cb, pathinfo, NULL);
	sqlite3_free(sql);

	if (sqlite_ret != SQLITE_OK) {
		APPFS_DEBUG("Call to sqlite3_exec failed.");

		return(-EIO);
	}

	if (pathinfo->type == APPFS_PATHTYPE_INVALID) {
		return(-ENOENT);
	}

	return(0);
}

/* Get information about a path, and optionally list children */
static int appfs_get_path_info(const char *_path, struct appfs_pathinfo *pathinfo, struct appfs_children **children) {
	struct appfs_site *sites, *site;
	struct appfs_children *node, *dir_children;
	struct appfs_package *packages, *package;
	appfs_os_t os_val;
	appfs_cpuArch_t cpuArch_val;
	char *hostname, *packagename, *os_cpuArch, *os, *cpuArch, *version;
	char *path, *path_s;
	char *package_hash;
	int sites_count, packages_count, os_cpuArch_count, version_count, files_count;
	int fileinfo_ret;

	if (children) {
		*children = NULL;
	}

	if (_path == NULL) {
		return(-ENOENT);
	}

	if (_path[0] != '/') {
		return(-ENOENT);
	}

	if (_path[1] == '\0') {
		/* Request for the root directory */
		sites = appfs_getsites(&sites_count);

		pathinfo->type = APPFS_PATHTYPE_DIRECTORY;
		pathinfo->typeinfo.dir.childcount = sites_count;
		pathinfo->hostname[0] = '\0';
		pathinfo->time = globalThread.boottime;

		if (children) {
			for (site = sites; site; site = site->_next) {
				node = (void *) ckalloc(sizeof(*node));
				node->_next = *children;
				strcpy(node->name, site->name);
				*children = node;
			}
		}

		appfs_free_list_site(sites);

		return(0);
	}

	path = strdup(_path);
	path_s = path;

	hostname = path + 1;
	packagename = strchr(hostname, '/');

	if (packagename != NULL) {
		*packagename = '\0';
		packagename++;
	}

	snprintf(pathinfo->hostname, sizeof(pathinfo->hostname), "%s", hostname);

	packages = appfs_getindex(hostname, &packages_count);

	if (packages == NULL || packages_count == 0) {
		APPFS_DEBUG("Unable to fetch package index from %s", hostname);

		free(path_s);

		return(-ENOENT);
	}

	if (packagename == NULL) {
		/* Request for a single hostname */
		pathinfo->type = APPFS_PATHTYPE_DIRECTORY;
		pathinfo->typeinfo.dir.childcount = packages_count;
		pathinfo->time = globalThread.boottime;

		if (children) {
			for (package = packages; package; package = package->_next) {
				node = (void *) ckalloc(sizeof(*node));
				node->_next = *children;
				strcpy(node->name, package->name);
				*children = node;
			}
		}

		appfs_free_list_package(packages);

		free(path_s);

		return(0);
	}

	os_cpuArch = strchr(packagename, '/');
	if (os_cpuArch == NULL) {
		/* Request for OS and CPU Arch for a specific package */
		pathinfo->type = APPFS_PATHTYPE_DIRECTORY;
		pathinfo->time = globalThread.boottime;

		os_cpuArch_count = 0;
		for (package = packages; package; package = package->_next) {
			if (strcmp(package->name, packagename) != 0) {
				APPFS_DEBUG("Skipping package named \"%s\", does not match requested package: \"%s\"", package->name, packagename);

				continue;
			}

			os_cpuArch_count++;

			if (children) {
				node = (void *) ckalloc(sizeof(*node));
				node->_next = *children;
				snprintf(node->name, sizeof(node->name), "%s-%s", package->os_str, package->cpuArch_str);
				*children = node;
			}
		}

		appfs_free_list_package(packages);

		free(path_s);

		pathinfo->typeinfo.dir.childcount = os_cpuArch_count;

		return(0);
	}

	*os_cpuArch = '\0';
	os_cpuArch++;

	version = strchr(os_cpuArch, '/');

	if (version != NULL) {
		*version = '\0';
		version++;
	}

	os = os_cpuArch;
	cpuArch = strchr(os_cpuArch, '-');
	if (cpuArch) {
		*cpuArch = '\0';
		cpuArch++;

		cpuArch_val = appfs_convert_cpuArch_fromString(cpuArch);
	} else {
		cpuArch_val = APPFS_CPU_UNKNOWN;
	}

	os_val = appfs_convert_os_fromString(os);

	if (version == NULL) {
		/* Request for version list for a package on an OS/CPU */
		pathinfo->type = APPFS_PATHTYPE_DIRECTORY;
		pathinfo->time = globalThread.boottime;

		version_count = 0;
		for (package = packages; package; package = package->_next) {
			if (strcmp(package->name, packagename) != 0) {
				APPFS_DEBUG("Skipping package named \"%s\", does not match requested package: \"%s\"", package->name, packagename);

				continue;
			}

			if (package->os != os_val) {
				continue;
			}

			if (package->cpuArch != cpuArch_val) {
				continue;
			}

			version_count++;

			if (children) {
				node = (void *) ckalloc(sizeof(*node));
				node->_next = *children;
				strcpy(node->name, package->version);
				*children = node;
			}
		}

		appfs_free_list_package(packages);

		free(path_s);

		pathinfo->typeinfo.dir.childcount = version_count;

		return(0);
	}

	appfs_free_list_package(packages);

	path = strchr(version, '/');
	if (path == NULL) {
		path = "";
	} else {
		*path = '\0';
		path++;
	}

	/* Request for a file in a specific package */
	APPFS_DEBUG("Requesting information for hostname = %s, package = %s, os = %s, cpuArch = %s, version = %s, path = %s", 
		hostname, packagename, os, cpuArch, version, path
	);

	package_hash = appfs_lookup_package_hash(hostname, packagename, os, cpuArch, version);
	if (package_hash == NULL) {
		free(path_s);

		return(-ENOENT);
	}

	APPFS_DEBUG("  ... which hash a hash of %s", package_hash);

	if (strcmp(path, "") == 0) {
		pathinfo->type = APPFS_PATHTYPE_DIRECTORY;
		pathinfo->time = globalThread.boottime;
	} else {
		fileinfo_ret = appfs_getfileinfo(hostname, package_hash, path, pathinfo);
		if (fileinfo_ret != 0) {
			free(path_s);

			return(fileinfo_ret);
		}
	}

	if (pathinfo->type == APPFS_PATHTYPE_DIRECTORY) {
		dir_children = appfs_getchildren(hostname, package_hash, path, &files_count);

		if (dir_children != NULL) {
			pathinfo->typeinfo.dir.childcount = files_count;
		}

		if (children) {
			*children = dir_children;
		}
	}

	free(path_s);

	return(0);
}

static int appfs_fuse_getattr(const char *path, struct stat *stbuf) {
	struct appfs_pathinfo pathinfo;
	int res = 0;

	APPFS_DEBUG("Enter (path = %s, ...)", path);

	res = appfs_get_path_info(path, &pathinfo, NULL);
	if (res != 0) {
		return(res);
	}

	memset(stbuf, 0, sizeof(struct stat));

	stbuf->st_mtime = pathinfo.time;
	stbuf->st_ctime = pathinfo.time;
	stbuf->st_atime = pathinfo.time;

	if (pathinfo.type == APPFS_PATHTYPE_DIRECTORY) {
		stbuf->st_mode = S_IFDIR | 0555;
		stbuf->st_nlink = 2 + pathinfo.typeinfo.dir.childcount;
	} else {
		if (pathinfo.typeinfo.file.executable) {
			stbuf->st_mode = S_IFREG | 0555;
		} else {
			stbuf->st_mode = S_IFREG | 0444;
		}

		stbuf->st_nlink = 1;
		stbuf->st_size = pathinfo.typeinfo.file.size;
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

#ifdef APPFS_TEST_DRIVER
static int appfs_test_driver(void) {
	struct appfs_site *sites, *site;
	struct appfs_package *packages, *package;
	struct appfs_children *files, *file;
	char *sha1 = NULL;
	int packages_count = 0, sites_count = 0, files_count;

	sites = appfs_getsites(&sites_count);
	printf("Sites:\n");
	for (site = sites; site; site = site->_next) {
		printf("\tname = %s\n", site->name);
	}

	appfs_free_list_site(sites);

	packages = appfs_getindex("rkeene.org", &packages_count);
	if (packages == NULL || packages_count == 0) {
		fprintf(stderr, "Unable to fetch package index from rkeene.org.\n");

		return(1);
	}

	for (package = packages; package; package = package->_next) {
		sha1 = package->sha1;

		printf("Package:\n\tname = %s\n\tversion = %s\n\tsha1 = %s\n\tos = %s\n\tcpuArch = %s\n",
			package->name,
			package->version,
			package->sha1,
			appfs_convert_os_toString(package->os),
			appfs_convert_cpuArch_toString(package->cpuArch)
		);
	}

	files = appfs_getchildren("rkeene.org", sha1, "", &files_count);
	if (files == NULL) {
		fprintf(stderr, "Unable to list files in the last package.\n");

		return(1);
	}

	printf("Files:\n");
	for (file = files; file; file = file->_next) {
		printf("\t%s\n", file->name);
	}

	appfs_free_list_children(files);

	files = appfs_getchildren("rkeene.org", sha1, "tcl", &files_count);
	if (files == NULL) {
		fprintf(stderr, "Unable to list files in the last package.\n");

		return(1);
	}

	printf("Files:\n");
	for (file = files; file; file = file->_next) {
		printf("\ttcl/%s\n", file->name);
	}

	appfs_free_list_children(files);
	appfs_free_list_package(packages);

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
	int pthread_ret, snprintf_ret, sqlite_ret;

	globalThread.cachedir = cachedir;
	globalThread.boottime = time(NULL);

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
 
