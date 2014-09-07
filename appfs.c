#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

static int appfs_getattr(const char *path, struct stat *stbuf) {
	int res = 0;

	memset(stbuf, 0, sizeof(struct stat));
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else {
		res = -ENOENT;
	}

	return res;
}

static int appfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
	if (strcmp(path, "/") != 0) {
		return(-ENOENT);
	}

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	return 0;
}

static int appfs_open(const char *path, struct fuse_file_info *fi) {
	return(-ENOENT);

	if ((fi->flags & 3) != O_RDONLY)
		return -EACCES;

	return 0;
}

static int appfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	return(-ENOENT);
}

static struct fuse_operations appfs_oper = {
	.getattr	= appfs_getattr,
	.readdir	= appfs_readdir,
	.open		= appfs_open,
	.read		= appfs_read,
};

int main(int argc, char **argv) {
	return(fuse_main(argc, argv, &appfs_oper, NULL));
}
 
