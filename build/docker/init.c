#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

int run(const char *path, ...) {
	va_list ap;
	char **argv;
	int argvMax, argvIdx;
	pid_t pid;
	int pidstat;

	pid = fork();
	if (pid == ((pid_t) -1)) {
		return(-1);
	}
	
	if (pid != 0) {
		waitpid(pid, &pidstat, 0);

		return(pidstat);
	}

	argvMax = 32;
	argv = malloc(sizeof(*argv) * argvMax);

	va_start(ap, path);

	for (argvIdx = 0; argvIdx < argvMax; argvIdx++) {
		argv[argvIdx] = va_arg(ap, char *);
		if (argv[argvIdx] == NULL) {
			break;
		}
	}
	
	va_end(ap);

	execv(path, argv);

	exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
	if (access("/dev/fuse", F_OK) != 0) {
		fprintf(stderr, "This container needs to be run as:  docker run --cap-add SYS_ADMIN --device /dev/fuse ...\n");

		return(1);
	}

	mkdir("/bin", 0755);
	mkdir("/lib", 0755);
	mkdir("/opt", 0755);
	mkdir("/opt/appfs", 0755);
	mkdir("/var", 0755);
	mkdir("/var/cache", 0755);
	mkdir("/var/cache/appfs", 0755);
	run("/bin/appfsd", "appfsd", "/var/cache/appfs", "/opt/appfs", NULL);

	symlink(".", "/usr");
	symlink("lib", "/lib64");

	symlink("/opt/appfs/core.appfs.rkeene.org/bash/platform/latest/bin/bash", "/bin/bash");
	symlink("/opt/appfs/core.appfs.rkeene.org/coreutils/platform/latest/bin/env", "/bin/env");

	symlink("/bin/bash", "/bin/sh");

	setenv("PATH", "/bin:/opt/appfs/core.appfs.rkeene.org/coreutils/platform/latest/bin", 1);
	run("/bin/appfs-cache", "appfs-cache", "install", "-lib", "core.appfs.rkeene.org", "glibc", NULL);
	run("/bin/appfs-cache", "appfs-cache", "install", "core.appfs.rkeene.org", "coreutils", NULL);
	setenv("PATH", "/bin", 1);

	if (argc == 1) {
		run("/bin/sh", "sh", NULL);
	} else {
		argv++;
		execvp(argv[0], argv);
	}

	return(0);
}
