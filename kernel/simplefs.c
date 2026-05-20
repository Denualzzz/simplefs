#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/cred.h>
#include <linux/crc32.h>
#include <linux/dirent.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/parser.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/uaccess.h>

#include "simplefs_ioctl.h"

#define SIMPLEFS_MAGIC 0x53465331U
#define SIMPLEFS_VERSION 1U
#define SIMPLEFS_BLOCK_SIZE 512U
#define SIMPLEFS_ROOT_INO 1UL
#define SIMPLEFS_FIRST_FILE_INO 2UL
#define SIMPLEFS_DEFAULT_NAME_LEN 32U
#define SIMPLEFS_DEFAULT_FILE_SECTORS 1U

static char *disk_name;
static unsigned long sb_primary_sector;
static unsigned long sb_backup_sector = 1;
static unsigned int max_name_len = SIMPLEFS_DEFAULT_NAME_LEN;
static unsigned int max_file_sectors = SIMPLEFS_DEFAULT_FILE_SECTORS;

module_param(disk_name, charp, 0444);
MODULE_PARM_DESC(disk_name, "Expected block device name used for SimpleFS");
module_param(sb_primary_sector, ulong, 0444);
MODULE_PARM_DESC(sb_primary_sector, "Primary superblock sector offset");
module_param(sb_backup_sector, ulong, 0444);
MODULE_PARM_DESC(sb_backup_sector, "Backup superblock sector offset");
module_param(max_name_len, uint, 0444);
MODULE_PARM_DESC(max_name_len, "Maximum generated file name length");
module_param(max_file_sectors, uint, 0444);
MODULE_PARM_DESC(max_file_sectors, "Maximum file size in sectors");

struct simplefs_disk_super {
	__le32 magic;
	__le32 version;
	__le64 total_sectors;
	__le64 sb_primary_sector;
	__le64 sb_backup_sector;
	__le32 sector_size;
	__le32 max_name_len;
	__le32 max_file_sectors;
	__le64 data_sectors;
	__le64 file_count;
	__le32 crc32;
	__u8 reserved[448];
} __packed;

struct simplefs_sb_info {
	u64 total_sectors;
	u64 data_sectors;
	u64 file_count;
	u64 sb_primary_sector;
	u64 sb_backup_sector;
	u32 max_name_len;
	u32 max_file_sectors;
};

static const struct super_operations simplefs_super_ops;
static const struct inode_operations simplefs_dir_iops;
static const struct file_operations simplefs_dir_fops;
static const struct file_operations simplefs_file_fops;

static u32 simplefs_disk_super_crc(const struct simplefs_disk_super *disk_sb)
{
	struct simplefs_disk_super tmp = *disk_sb;

	tmp.crc32 = 0;
	return crc32(~0U, (const u8 *)&tmp, sizeof(tmp));
}

static u64 simplefs_min_u64(u64 a, u64 b)
{
	return a < b ? a : b;
}

static u64 simplefs_file_index(const struct inode *inode)
{
	return inode->i_ino - SIMPLEFS_FIRST_FILE_INO;
}

static int simplefs_make_name(char *buf, size_t len, u64 index)
{
	return scnprintf(buf, len, "f%llu", index);
}

static unsigned int simplefs_required_name_len(u64 file_count)
{
	u64 max_index = file_count ? file_count - 1 : 0;
	unsigned int digits = 1;

	while (max_index >= 10) {
		max_index /= 10;
		digits++;
	}

	return digits + 1;
}

static u32 simplefs_choose_file_sectors(u64 data_sectors,
					       u32 max_sectors)
{
	u32 sectors = (u32)simplefs_min_u64(data_sectors, max_sectors);

	while (sectors > 1) {
		if (data_sectors % sectors == 0)
			return sectors;
		sectors--;
	}

	return 1;
}

static sector_t simplefs_data_sector(const struct simplefs_sb_info *sbi, u64 ordinal)
{
	u64 first = simplefs_min_u64(sbi->sb_primary_sector, sbi->sb_backup_sector);
	u64 second = sbi->sb_primary_sector ^ sbi->sb_backup_sector ^ first;
	u64 sector = ordinal;

	if (sector >= first)
		sector++;
	if (sector >= second)
		sector++;

	return (sector_t)sector;
}

static u32 simplefs_file_sectors(const struct simplefs_sb_info *sbi, u64 file_index)
{
	if (file_index >= sbi->file_count)
		return 0;

	return sbi->max_file_sectors;
}

static loff_t simplefs_file_size(const struct simplefs_sb_info *sbi, u64 file_index)
{
	return (loff_t)simplefs_file_sectors(sbi, file_index) * SIMPLEFS_BLOCK_SIZE;
}

static int simplefs_map_file_sector(const struct simplefs_sb_info *sbi,
				    u64 file_index, u32 file_sector,
				    sector_t *disk_sector)
{
	u32 sectors = simplefs_file_sectors(sbi, file_index);
	u64 ordinal;

	if (file_sector >= sectors)
		return -EINVAL;

	ordinal = file_index * sbi->max_file_sectors + file_sector;
	*disk_sector = simplefs_data_sector(sbi, ordinal);
	return 0;
}

static int simplefs_read_disk_super(struct super_block *sb, u64 sector,
				    struct simplefs_disk_super *disk_sb)
{
	struct buffer_head *bh;

	bh = sb_bread(sb, sector);
	if (!bh)
		return -EIO;

	memcpy(disk_sb, bh->b_data, sizeof(*disk_sb));
	brelse(bh);
	return 0;
}

static int simplefs_write_disk_super(struct super_block *sb, u64 sector,
				     const struct simplefs_disk_super *disk_sb)
{
	struct buffer_head *bh;

	bh = sb_bread(sb, sector);
	if (!bh)
		return -EIO;

	memset(bh->b_data, 0, SIMPLEFS_BLOCK_SIZE);
	memcpy(bh->b_data, disk_sb, sizeof(*disk_sb));
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

static bool simplefs_validate_disk_super(const struct simplefs_disk_super *disk_sb,
					 const struct simplefs_sb_info *expected)
{
	u32 stored_crc = le32_to_cpu(disk_sb->crc32);

	if (le32_to_cpu(disk_sb->magic) != SIMPLEFS_MAGIC)
		return false;
	if (le32_to_cpu(disk_sb->version) != SIMPLEFS_VERSION)
		return false;
	if (le32_to_cpu(disk_sb->sector_size) != SIMPLEFS_BLOCK_SIZE)
		return false;
	if (stored_crc != simplefs_disk_super_crc(disk_sb))
		return false;
	if (le64_to_cpu(disk_sb->total_sectors) != expected->total_sectors)
		return false;
	if (le64_to_cpu(disk_sb->sb_primary_sector) != expected->sb_primary_sector)
		return false;
	if (le64_to_cpu(disk_sb->sb_backup_sector) != expected->sb_backup_sector)
		return false;
	if (le32_to_cpu(disk_sb->max_name_len) != expected->max_name_len)
		return false;
	if (le32_to_cpu(disk_sb->max_file_sectors) != expected->max_file_sectors)
		return false;
	if (le64_to_cpu(disk_sb->data_sectors) != expected->data_sectors)
		return false;
	if (le64_to_cpu(disk_sb->file_count) != expected->file_count)
		return false;

	return true;
}

static void simplefs_prepare_disk_super(struct simplefs_disk_super *disk_sb,
				       const struct simplefs_sb_info *sbi)
{
	memset(disk_sb, 0, sizeof(*disk_sb));
	disk_sb->magic = cpu_to_le32(SIMPLEFS_MAGIC);
	disk_sb->version = cpu_to_le32(SIMPLEFS_VERSION);
	disk_sb->total_sectors = cpu_to_le64(sbi->total_sectors);
	disk_sb->sb_primary_sector = cpu_to_le64(sbi->sb_primary_sector);
	disk_sb->sb_backup_sector = cpu_to_le64(sbi->sb_backup_sector);
	disk_sb->sector_size = cpu_to_le32(SIMPLEFS_BLOCK_SIZE);
	disk_sb->max_name_len = cpu_to_le32(sbi->max_name_len);
	disk_sb->max_file_sectors = cpu_to_le32(sbi->max_file_sectors);
	disk_sb->data_sectors = cpu_to_le64(sbi->data_sectors);
	disk_sb->file_count = cpu_to_le64(sbi->file_count);
	disk_sb->crc32 = cpu_to_le32(simplefs_disk_super_crc(disk_sb));
}

static int simplefs_sync_superblocks(struct super_block *sb,
				     const struct simplefs_sb_info *sbi)
{
	struct simplefs_disk_super disk_sb;
	int ret;

	simplefs_prepare_disk_super(&disk_sb, sbi);
	ret = simplefs_write_disk_super(sb, sbi->sb_primary_sector, &disk_sb);
	if (ret)
		return ret;

	return simplefs_write_disk_super(sb, sbi->sb_backup_sector, &disk_sb);
}

static int simplefs_load_or_init_superblocks(struct super_block *sb,
					     struct simplefs_sb_info *sbi)
{
	struct simplefs_disk_super primary;
	struct simplefs_disk_super backup;
	bool primary_ok = false;
	bool backup_ok = false;
	int ret;

	ret = simplefs_read_disk_super(sb, sbi->sb_primary_sector, &primary);
	if (!ret)
		primary_ok = simplefs_validate_disk_super(&primary, sbi);

	ret = simplefs_read_disk_super(sb, sbi->sb_backup_sector, &backup);
	if (!ret)
		backup_ok = simplefs_validate_disk_super(&backup, sbi);

	if (!primary_ok && !backup_ok)
		pr_info("simplefs: initializing new filesystem\n");
	else if (!primary_ok || !backup_ok)
		pr_warn("simplefs: repairing invalid superblock copy\n");

	if (!primary_ok || !backup_ok)
		return simplefs_sync_superblocks(sb, sbi);

	return 0;
}

static int simplefs_parse_name(const struct qstr *name, u64 *index)
{
	char tmp[SIMPLEFS_IOCTL_NAME_MAX + 1];

	if (name->len < 2 || name->len > SIMPLEFS_IOCTL_NAME_MAX)
		return -ENOENT;
	if (name->name[0] != 'f')
		return -ENOENT;

	memcpy(tmp, name->name, name->len);
	tmp[name->len] = '\0';
	if (kstrtoull(tmp + 1, 10, index))
		return -ENOENT;

	return 0;
}

static struct inode *simplefs_get_inode(struct super_block *sb,
				       struct inode *dir, umode_t mode, u64 ino)
{
	struct inode *inode;
	struct timespec64 now;
	struct simplefs_sb_info *sbi = sb->s_fs_info;

	inode = new_inode(sb);
	if (!inode)
		return NULL;

	inode->i_ino = ino;
	inode->i_mode = mode;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	now = current_time(inode);
	inode_set_atime_to_ts(inode, now);
	inode_set_mtime_to_ts(inode, now);
	inode_set_ctime_to_ts(inode, now);

	if (S_ISDIR(mode)) {
		inode->i_op = &simplefs_dir_iops;
		inode->i_fop = &simplefs_dir_fops;
		inc_nlink(inode);
	} else if (S_ISREG(mode)) {
		u64 file_index = ino - SIMPLEFS_FIRST_FILE_INO;

		inode->i_fop = &simplefs_file_fops;
		inode->i_size = simplefs_file_size(sbi, file_index);
		inode->i_blocks = simplefs_file_sectors(sbi, file_index);
	}

	return inode;
}

static struct dentry *simplefs_lookup(struct inode *dir, struct dentry *dentry,
				      unsigned int flags)
{
	struct simplefs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct inode *inode = NULL;
	u64 index;
	int ret;

	if (dentry->d_name.len > sbi->max_name_len)
		return ERR_PTR(-ENAMETOOLONG);

	ret = simplefs_parse_name(&dentry->d_name, &index);
	if (!ret && index < sbi->file_count) {
		inode = simplefs_get_inode(dir->i_sb, dir, S_IFREG | 0644,
						SIMPLEFS_FIRST_FILE_INO + index);
		if (!inode)
			return ERR_PTR(-ENOMEM);
	}

	return d_splice_alias(inode, dentry);
}

static int simplefs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct simplefs_sb_info *sbi = inode->i_sb->s_fs_info;
	u64 index;

	if (!dir_emit_dots(file, ctx))
		return 0;

	for (index = ctx->pos - 2; index < sbi->file_count; index++) {
		char name[SIMPLEFS_IOCTL_NAME_MAX + 1];
		int len = simplefs_make_name(name, sizeof(name), index);

		if (len > sbi->max_name_len)
			return -ENAMETOOLONG;
		if (!dir_emit(ctx, name, len, SIMPLEFS_FIRST_FILE_INO + index,
			      DT_REG))
			return 0;
		ctx->pos = index + 3;
	}

	return 0;
}

static ssize_t simplefs_read(struct file *file, char __user *buf, size_t len,
				 loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct simplefs_sb_info *sbi = inode->i_sb->s_fs_info;
	u64 index = simplefs_file_index(inode);
	loff_t size = simplefs_file_size(sbi, index);
	ssize_t done = 0;

	if (*ppos >= size)
		return 0;
	if (len > size - *ppos)
		len = size - *ppos;

	while (len) {
		sector_t disk_sector;
		struct buffer_head *bh;
		u32 file_sector = div_u64((u64)*ppos, SIMPLEFS_BLOCK_SIZE);
		u32 sector_offset = (u32)((u64)*ppos % SIMPLEFS_BLOCK_SIZE);
		size_t chunk = min_t(size_t, len, SIMPLEFS_BLOCK_SIZE - sector_offset);
		int ret;

		ret = simplefs_map_file_sector(sbi, index, file_sector, &disk_sector);
		if (ret)
			return done ? done : ret;

		bh = sb_bread(inode->i_sb, disk_sector);
		if (!bh)
			return done ? done : -EIO;
		if (copy_to_user(buf + done, bh->b_data + sector_offset, chunk)) {
			brelse(bh);
			return done ? done : -EFAULT;
		}
		brelse(bh);

		len -= chunk;
		*ppos += chunk;
		done += chunk;
	}

	return done;
}

static ssize_t simplefs_write(struct file *file, const char __user *buf,
				  size_t len, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct simplefs_sb_info *sbi = inode->i_sb->s_fs_info;
	u64 index = simplefs_file_index(inode);
	loff_t size = simplefs_file_size(sbi, index);
	ssize_t done = 0;

	if (*ppos >= size)
		return -ENOSPC;
	if (len > size - *ppos)
		len = size - *ppos;

	while (len) {
		sector_t disk_sector;
		struct buffer_head *bh;
		u32 file_sector = div_u64((u64)*ppos, SIMPLEFS_BLOCK_SIZE);
		u32 sector_offset = (u32)((u64)*ppos % SIMPLEFS_BLOCK_SIZE);
		size_t chunk = min_t(size_t, len, SIMPLEFS_BLOCK_SIZE - sector_offset);
		int ret;

		ret = simplefs_map_file_sector(sbi, index, file_sector, &disk_sector);
		if (ret)
			return done ? done : ret;

		bh = sb_bread(inode->i_sb, disk_sector);
		if (!bh)
			return done ? done : -EIO;
		if (copy_from_user(bh->b_data + sector_offset, buf + done, chunk)) {
			brelse(bh);
			return done ? done : -EFAULT;
		}
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);

		len -= chunk;
		*ppos += chunk;
		done += chunk;
	}

	return done;
}

static int simplefs_zero_sector(struct super_block *sb, sector_t sector)
{
	struct buffer_head *bh;

	bh = sb_bread(sb, sector);
	if (!bh)
		return -EIO;

	memset(bh->b_data, 0, SIMPLEFS_BLOCK_SIZE);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

static int simplefs_zero_files(struct super_block *sb)
{
	struct simplefs_sb_info *sbi = sb->s_fs_info;
	u64 ordinal;

	for (ordinal = 0; ordinal < sbi->data_sectors; ordinal++) {
		int ret = simplefs_zero_sector(sb, simplefs_data_sector(sbi, ordinal));

		if (ret)
			return ret;
	}

	return 0;
}

static int simplefs_erase_fs(struct super_block *sb)
{
	struct simplefs_sb_info *sbi = sb->s_fs_info;
	u64 sector;

	for (sector = 0; sector < sbi->total_sectors; sector++) {
		int ret = simplefs_zero_sector(sb, sector);

		if (ret)
			return ret;
	}

	return 0;
}

static int simplefs_file_crc(struct super_block *sb, u64 index, u32 *crc)
{
	struct simplefs_sb_info *sbi = sb->s_fs_info;
	u32 sectors = simplefs_file_sectors(sbi, index);
	u32 value = ~0U;
	u32 i;

	for (i = 0; i < sectors; i++) {
		sector_t disk_sector;
		struct buffer_head *bh;
		int ret;

		ret = simplefs_map_file_sector(sbi, index, i, &disk_sector);
		if (ret)
			return ret;

		bh = sb_bread(sb, disk_sector);
		if (!bh)
			return -EIO;
		value = crc32(value, bh->b_data, SIMPLEFS_BLOCK_SIZE);
		brelse(bh);
	}

	*crc = value;
	return 0;
}

static long simplefs_ioctl_hashes(struct super_block *sb, unsigned long arg)
{
	struct simplefs_sb_info *sbi = sb->s_fs_info;
	struct simplefs_hashes request;
	u64 i;
	u64 limit;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;

	request.count = sbi->file_count;
	limit = simplefs_min_u64(request.capacity, sbi->file_count);

	for (i = 0; i < limit; i++) {
		struct simplefs_file_hash item;
		char __user *dst = (char __user *)(unsigned long)request.entries;
		int ret;

		memset(&item, 0, sizeof(item));
		item.file_index = i;
		item.sectors = simplefs_file_sectors(sbi, i);
		simplefs_make_name(item.name, sizeof(item.name), i);
		ret = simplefs_file_crc(sb, i, &item.crc32);
		if (ret)
			return ret;

		if (copy_to_user(dst + i * sizeof(item), &item, sizeof(item)))
			return -EFAULT;
	}

	if (copy_to_user((void __user *)arg, &request, sizeof(request)))
		return -EFAULT;

	return 0;
}

static long simplefs_ioctl_mapping(struct super_block *sb, unsigned long arg)
{
	struct simplefs_sb_info *sbi = sb->s_fs_info;
	struct simplefs_mapping request;
	struct qstr name;
	u64 index;
	u32 sectors;
	u64 i;
	u64 limit;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;

	request.name[SIMPLEFS_IOCTL_NAME_MAX] = '\0';
	name.name = request.name;
	name.len = strnlen(request.name, SIMPLEFS_IOCTL_NAME_MAX);
	if (name.len > sbi->max_name_len)
		return -ENAMETOOLONG;
	if (simplefs_parse_name(&name, &index) || index >= sbi->file_count)
		return -ENOENT;

	sectors = simplefs_file_sectors(sbi, index);
	request.file_index = index;
	request.sectors = sectors;
	limit = simplefs_min_u64(request.sector_capacity, sectors);

	for (i = 0; i < limit; i++) {
		sector_t disk_sector;
		__u64 user_sector;
		char __user *dst = (char __user *)(unsigned long)request.sectors_ptr;
		int ret;

		ret = simplefs_map_file_sector(sbi, index, i, &disk_sector);
		if (ret)
			return ret;
		user_sector = disk_sector;
		if (copy_to_user(dst + i * sizeof(user_sector), &user_sector,
				 sizeof(user_sector)))
			return -EFAULT;
	}

	if (copy_to_user((void __user *)arg, &request, sizeof(request)))
		return -EFAULT;

	return 0;
}

static long simplefs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct super_block *sb = file_inode(file)->i_sb;

	switch (cmd) {
	case SIMPLEFS_IOCTL_ZERO_FILES:
		return simplefs_zero_files(sb);
	case SIMPLEFS_IOCTL_ERASE_FS:
		return simplefs_erase_fs(sb);
	case SIMPLEFS_IOCTL_GET_HASHES:
		return simplefs_ioctl_hashes(sb, arg);
	case SIMPLEFS_IOCTL_GET_MAPPING:
		return simplefs_ioctl_mapping(sb, arg);
	default:
		return -ENOTTY;
	}
}

static int simplefs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct simplefs_sb_info *sbi = dentry->d_sb->s_fs_info;

	buf->f_type = SIMPLEFS_MAGIC;
	buf->f_bsize = SIMPLEFS_BLOCK_SIZE;
	buf->f_blocks = sbi->total_sectors;
	buf->f_bfree = 0;
	buf->f_bavail = 0;
	buf->f_files = sbi->file_count + 1;
	buf->f_ffree = 0;
	buf->f_namelen = sbi->max_name_len;
	return 0;
}

static void simplefs_put_super(struct super_block *sb)
{
	kfree(sb->s_fs_info);
	sb->s_fs_info = NULL;
}

static int simplefs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct simplefs_sb_info *sbi;
	struct inode *root;
	u64 total_sectors;
	unsigned int needed_name_len;
	int ret;

	if (sb_primary_sector == sb_backup_sector)
		return -EINVAL;
	if (!max_file_sectors || max_name_len > SIMPLEFS_IOCTL_NAME_MAX)
		return -EINVAL;
	if (!sb_set_blocksize(sb, SIMPLEFS_BLOCK_SIZE))
		return -EINVAL;

	total_sectors = bdev_nr_sectors(sb->s_bdev);
	if (total_sectors <= 2)
		return -EINVAL;
	if (sb_primary_sector >= total_sectors || sb_backup_sector >= total_sectors)
		return -EINVAL;

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	sbi->total_sectors = total_sectors;
	sbi->sb_primary_sector = sb_primary_sector;
	sbi->sb_backup_sector = sb_backup_sector;
	sbi->max_name_len = max_name_len;
	sbi->data_sectors = total_sectors - 2;
	sbi->max_file_sectors = simplefs_choose_file_sectors(sbi->data_sectors,
							       max_file_sectors);
	sbi->file_count = div_u64(sbi->data_sectors, sbi->max_file_sectors);

	needed_name_len = simplefs_required_name_len(sbi->file_count);
	if (needed_name_len > max_name_len) {
		ret = -ENAMETOOLONG;
		goto fail;
	}

	sb->s_magic = SIMPLEFS_MAGIC;
	sb->s_op = &simplefs_super_ops;
	sb->s_fs_info = sbi;

	ret = simplefs_load_or_init_superblocks(sb, sbi);
	if (ret)
		goto fail;

	root = simplefs_get_inode(sb, NULL, S_IFDIR | 0755, SIMPLEFS_ROOT_INO);
	if (!root) {
		ret = -ENOMEM;
		goto fail;
	}

	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto fail;
	}

	return 0;

fail:
	kfree(sbi);
	sb->s_fs_info = NULL;
	return ret;
}

static struct dentry *simplefs_mount(struct file_system_type *fs_type,
				     int flags, const char *dev_name, void *data)
{
	if (disk_name && (!dev_name || strcmp(disk_name, dev_name))) {
		pr_err("simplefs: expected device %s, got %s\n", disk_name,
		       dev_name ? dev_name : "<none>");
		return ERR_PTR(-EINVAL);
	}

	return mount_bdev(fs_type, flags, dev_name, data, simplefs_fill_super);
}

static const struct super_operations simplefs_super_ops = {
	.statfs = simplefs_statfs,
	.drop_inode = generic_delete_inode,
	.put_super = simplefs_put_super,
};

static const struct inode_operations simplefs_dir_iops = {
	.lookup = simplefs_lookup,
};

static const struct file_operations simplefs_dir_fops = {
	.owner = THIS_MODULE,
	.iterate_shared = simplefs_readdir,
	.read = generic_read_dir,
	.llseek = generic_file_llseek,
	.unlocked_ioctl = simplefs_ioctl,
};

static const struct file_operations simplefs_file_fops = {
	.owner = THIS_MODULE,
	.read = simplefs_read,
	.write = simplefs_write,
	.llseek = generic_file_llseek,
	.unlocked_ioctl = simplefs_ioctl,
};

static struct file_system_type simplefs_fs_type = {
	.owner = THIS_MODULE,
	.name = "simplefs",
	.mount = simplefs_mount,
	.kill_sb = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV,
};

static int __init simplefs_init(void)
{
	BUILD_BUG_ON(sizeof(struct simplefs_disk_super) != SIMPLEFS_BLOCK_SIZE);
	return register_filesystem(&simplefs_fs_type);
}

static void __exit simplefs_exit(void)
{
	unregister_filesystem(&simplefs_fs_type);
}

module_init(simplefs_init);
module_exit(simplefs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OpenCode");
MODULE_DESCRIPTION("Simple sector-mapped VFS filesystem for Linux 6.12");
