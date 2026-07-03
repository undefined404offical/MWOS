#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "drivers/fs/vfs.h"

#define EXT2_MAGIC              0xEF53
#define EXT2_ROOT_INO           2

#define EXT2_S_IFIFO    0x1000
#define EXT2_S_IFCHR    0x2000
#define EXT2_S_IFDIR    0x4000
#define EXT2_S_IFBLK    0x6000
#define EXT2_S_IFREG    0x8000
#define EXT2_S_IFLNK    0xA000
#define EXT2_S_IFSOCK   0xC000

#define EXT2_S_ISREG(m)  (((m) & 0xF000) == 0x8000)
#define EXT2_S_ISDIR(m)  (((m) & 0xF000) == 0x4000)
#define EXT2_S_ISLNK(m)  (((m) & 0xF000) == 0xA000)

#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_FIFO     5
#define EXT2_FT_SOCK     6
#define EXT2_FT_SYMLINK  7

// EXT2 超级块
typedef struct __attribute__((packed)) {
    uint32_t  inodes_count;
    uint32_t  blocks_count;
    uint32_t  reserved_blocks_count;
    uint32_t  free_blocks_count;
    uint32_t  free_inodes_count;
    uint32_t  first_data_block;
    uint32_t  log_block_size;
    uint32_t  log_frag_size;
    uint32_t  blocks_per_group;
    uint32_t  frags_per_group;
    uint32_t  inodes_per_group;
    uint32_t  mtime;
    uint32_t  wtime;
    uint16_t  mnt_count;
    uint16_t  max_mnt_count;
    uint16_t  magic;
    uint16_t  state;
    uint16_t  errors;
    uint16_t  minor_rev_level;
    uint32_t  lastcheck;
    uint32_t  checkinterval;
    uint32_t  creator_os;
    uint32_t  rev_level;
    uint16_t  def_resuid;
    uint16_t  def_resgid;

    // EXT2_DYNAMIC_REV 扩展字段
    uint32_t  first_ino;
    uint16_t  inode_size;
    uint16_t  block_group_nr;
    uint32_t  feature_compat;
    uint32_t  feature_incompat;
    uint32_t  feature_ro_compat;
    uint8_t   uuid[16];
    uint8_t   volume_name[16];
    uint8_t   last_mounted[64];
    uint32_t  algo_bitmap;
} ext2_superblock_t;

_Static_assert(sizeof(ext2_superblock_t) == 204, "ext2 superblock must be 204 bytes");

// 块组描述符
typedef struct __attribute__((packed)) {
    uint32_t  block_bitmap;
    uint32_t  inode_bitmap;
    uint32_t  inode_table;
    uint16_t  free_blocks_count;
    uint16_t  free_inodes_count;
    uint16_t  used_dirs_count;
    uint16_t  pad;
    uint32_t  reserved[3];
} ext2_bg_desc_t;

// EXT2 inode（128 字节）
typedef struct __attribute__((packed)) {
    uint16_t  mode;
    uint16_t  uid;
    uint32_t  size;
    uint32_t  atime;
    uint32_t  ctime;
    uint32_t  mtime;
    uint32_t  dtime;
    uint16_t  gid;
    uint16_t  links_count;
    uint32_t  blocks;
    uint32_t  flags;
    uint32_t  osd1;
    uint32_t  block[15];   // 12 direct + 1 indirect + 1 double indirect + 1 triple
    uint32_t  generation;
    uint32_t  file_acl;
    uint32_t  dir_acl;
    uint32_t  faddr;
    uint32_t  osd2[3];
} ext2_inode_t;

_Static_assert(sizeof(ext2_inode_t) == 128, "ext2 inode must be 128 bytes");

// EXT2 目录项
typedef struct __attribute__((packed)) {
    uint32_t  inode;
    uint16_t  rec_len;
    uint8_t   name_len;
    uint8_t   file_type;
    char      name[0];     // 变长
} ext2_dir_entry_t;

// EXT2 内部句柄
typedef struct {
    uint32_t    inode_no;
    ext2_inode_t inode;
    uint32_t    position;
    uint32_t    file_size;
    bool        is_directory;
    bool        is_open;

    // 目录遍历状态
    uint32_t    dir_block_idx;      // 当前块在 inode->block[] 中的索引
    uint32_t    dir_block_phys;     // 当前块的物理块号
    uint32_t    dir_offset;         // 当前块内的偏移（字节）
    uint8_t*    dir_block_data;     // 当前块的缓存数据
} ext2_handle_t;

// EXT2 文件系统信息
typedef struct {
    ext2_superblock_t sb;
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t bg_desc_count;
    uint32_t bg_desc_blocks;     // 块组描述符占用的块数
    uint32_t partition_start;    // 分区起始 LBA
} ext2_fs_info_t;

// API
bool ext2_mount(uint32_t partition_start);
void ext2_umount(void);
bool ext2_open(const char* path, vfs_file_t* file, vfs_mode_t mode);
bool ext2_read_all_fast(void* handle, void* buffer);
bool ext2_file_exists(const char* path);
bool ext2_create_file(const char* path);
bool ext2_create_dir(const char* path);
const char* ext2_get_error(void);
bool ext2_mounted(void);

// 分区检测（读取 MBR 找 EXT2 分区，或在 LBA 0 处检测超级块）
uint32_t ext2_detect(void);

// VFS 驱动注册
void ext2_register(void);

#endif
