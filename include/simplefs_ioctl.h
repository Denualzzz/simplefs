#ifndef SIMPLEFS_IOCTL_H
#define SIMPLEFS_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint64_t __u64;
typedef uint32_t __u32;
#endif

#define SIMPLEFS_IOCTL_MAGIC 'S'
#define SIMPLEFS_IOCTL_NAME_MAX 255

struct simplefs_file_hash {
	__u64 file_index;
	__u32 sectors;
	__u32 crc32;
	char name[SIMPLEFS_IOCTL_NAME_MAX + 1];
};

struct simplefs_hashes {
	__u64 count;
	__u64 capacity;
	__u64 entries;
};

struct simplefs_mapping {
	char name[SIMPLEFS_IOCTL_NAME_MAX + 1];
	__u64 file_index;
	__u32 sectors;
	__u32 reserved;
	__u64 sector_capacity;
	__u64 sectors_ptr;
};

#define SIMPLEFS_IOCTL_ZERO_FILES _IO(SIMPLEFS_IOCTL_MAGIC, 1)
#define SIMPLEFS_IOCTL_ERASE_FS _IO(SIMPLEFS_IOCTL_MAGIC, 2)
#define SIMPLEFS_IOCTL_GET_HASHES \
	_IOWR(SIMPLEFS_IOCTL_MAGIC, 3, struct simplefs_hashes)
#define SIMPLEFS_IOCTL_GET_MAPPING \
	_IOWR(SIMPLEFS_IOCTL_MAGIC, 4, struct simplefs_mapping)

#endif
