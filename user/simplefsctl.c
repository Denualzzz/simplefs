#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include "simplefs_ioctl.h"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s demo <mountpoint>\n"
		"  %s zero <mountpoint>\n"
		"  %s erase <mountpoint>\n"
		"  %s hashes <mountpoint>\n"
		"  %s mapping <mountpoint> <file>\n",
		prog, prog, prog, prog, prog);
}

static int open_mountpoint(const char *mountpoint)
{
	int fd = open(mountpoint, O_RDONLY | O_DIRECTORY);

	if (fd < 0)
		perror("open mountpoint");
	return fd;
}

static int ioctl_noarg(const char *mountpoint, unsigned long request)
{
	int fd = open_mountpoint(mountpoint);
	int ret;

	if (fd < 0)
		return 1;

	ret = ioctl(fd, request);
	if (ret < 0) {
		perror("ioctl");
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}

static uint64_t random_u64(void)
{
	uint64_t value;
	int fd = open("/dev/urandom", O_RDONLY);

	if (fd >= 0 && read(fd, &value, sizeof(value)) == (ssize_t)sizeof(value)) {
		close(fd);
		return value;
	}
	if (fd >= 0)
		close(fd);

	value = ((uint64_t)random() << 32) ^ (uint64_t)random();
	return value;
}

static int demo_file(const char *mountpoint, const char *name)
{
	char path[4096];
	uint64_t written = random_u64();
	uint64_t read_back = 0;
	int fd;

	if (snprintf(path, sizeof(path), "%s/%s", mountpoint, name) >=
	    (int)sizeof(path)) {
		fprintf(stderr, "path is too long: %s/%s\n", mountpoint, name);
		return 1;
	}

	fd = open(path, O_RDWR);
	if (fd < 0) {
		perror(path);
		return 1;
	}

	if (pwrite(fd, &written, sizeof(written), 0) != (ssize_t)sizeof(written)) {
		perror("pwrite");
		close(fd);
		return 1;
	}

	if (pread(fd, &read_back, sizeof(read_back), 0) != (ssize_t)sizeof(read_back)) {
		perror("pread");
		close(fd);
		return 1;
	}

	close(fd);

	if (written != read_back) {
		fprintf(stderr, "%s: mismatch written=%" PRIu64 " read=%" PRIu64 "\n",
			name, written, read_back);
		return 1;
	}

	return 0;
}

static int command_demo(const char *mountpoint)
{
	DIR *dir = opendir(mountpoint);
	struct dirent *entry;
	uint64_t checked = 0;
	int failed = 0;

	if (!dir) {
		perror("opendir");
		return 1;
	}

	while ((entry = readdir(dir)) != NULL) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (entry->d_name[0] != 'f')
			continue;

		if (demo_file(mountpoint, entry->d_name))
			failed = 1;
		else
			checked++;
	}

	closedir(dir);
	printf("checked files: %" PRIu64 "\n", checked);
	return failed;
}

static int command_hashes(const char *mountpoint)
{
	int fd = open_mountpoint(mountpoint);
	struct simplefs_hashes request = { 0 };
	struct simplefs_file_hash *entries;
	uint64_t i;

	if (fd < 0)
		return 1;

	if (ioctl(fd, SIMPLEFS_IOCTL_GET_HASHES, &request) < 0) {
		perror("ioctl hashes count");
		close(fd);
		return 1;
	}

	entries = calloc(request.count, sizeof(*entries));
	if (!entries && request.count) {
		perror("calloc");
		close(fd);
		return 1;
	}

	request.capacity = request.count;
	request.entries = (uint64_t)(uintptr_t)entries;
	if (ioctl(fd, SIMPLEFS_IOCTL_GET_HASHES, &request) < 0) {
		perror("ioctl hashes");
		free(entries);
		close(fd);
		return 1;
	}

	for (i = 0; i < request.count; i++) {
		printf("%s index=%" PRIu64 " sectors=%u crc32=%08x\n",
		       entries[i].name, entries[i].file_index, entries[i].sectors,
		       entries[i].crc32);
	}

	free(entries);
	close(fd);
	return 0;
}

static int command_mapping(const char *mountpoint, const char *name)
{
	int fd = open_mountpoint(mountpoint);
	struct simplefs_mapping request = { 0 };
	uint64_t *sectors;
	uint64_t i;

	if (fd < 0)
		return 1;

	strncpy(request.name, name, SIMPLEFS_IOCTL_NAME_MAX);
	if (ioctl(fd, SIMPLEFS_IOCTL_GET_MAPPING, &request) < 0) {
		perror("ioctl mapping count");
		close(fd);
		return 1;
	}

	sectors = calloc(request.sectors, sizeof(*sectors));
	if (!sectors && request.sectors) {
		perror("calloc");
		close(fd);
		return 1;
	}

	request.sector_capacity = request.sectors;
	request.sectors_ptr = (uint64_t)(uintptr_t)sectors;
	if (ioctl(fd, SIMPLEFS_IOCTL_GET_MAPPING, &request) < 0) {
		perror("ioctl mapping");
		free(sectors);
		close(fd);
		return 1;
	}

	printf("%s index=%" PRIu64 " sectors=%u:", request.name,
	       request.file_index, request.sectors);
	for (i = 0; i < request.sectors; i++)
		printf(" %" PRIu64, sectors[i]);
	printf("\n");

	free(sectors);
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}

	if (!strcmp(argv[1], "demo"))
		return command_demo(argv[2]);
	if (!strcmp(argv[1], "zero"))
		return ioctl_noarg(argv[2], SIMPLEFS_IOCTL_ZERO_FILES);
	if (!strcmp(argv[1], "erase"))
		return ioctl_noarg(argv[2], SIMPLEFS_IOCTL_ERASE_FS);
	if (!strcmp(argv[1], "hashes"))
		return command_hashes(argv[2]);
	if (!strcmp(argv[1], "mapping") && argc == 4)
		return command_mapping(argv[2], argv[3]);

	usage(argv[0]);
	return 1;
}
