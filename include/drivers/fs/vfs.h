#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define VFS_MAX_PATH    256
#define VFS_MAX_NAME    64

// 文件操作模式
typedef enum {
    VFS_READ,
    VFS_WRITE,
    VFS_APPEND,
    VFS_CREATE
} vfs_mode_t;

// VFS 目录项（通用表示）
typedef struct {
    char     name[VFS_MAX_NAME];   // 文件名
    uint32_t size;                  // 文件大小
    bool     is_directory;          // 是否为目录
    uint16_t mode;                  // 权限模式
} vfs_dirent_t;

// VFS 文件句柄（由底层 FS 填充具体数据）
typedef struct {
    uint32_t    inode;              // inode 编号
    uint32_t    file_size;
    uint32_t    position;
    bool        is_directory;
    bool        is_open;

    // 底层 FS 私有数据（opaque）
    void*       fs_private;

    // 操作回调（由 FS 驱动设置）
    bool (*read_fn)(void* priv, void* buffer, uint32_t size, uint32_t* bytes_read);
    bool (*write_fn)(void* priv, const void* buffer, uint32_t size, uint32_t* bytes_written);
    void (*close_fn)(void* priv);
    bool (*read_dir_fn)(void* priv, vfs_dirent_t* entry);
} vfs_file_t;

// 文件系统驱动操作表
typedef struct {
    bool (*mount)(uint32_t partition_start);
    void (*umount)(void);
    bool (*open)(const char* path, vfs_file_t* file, vfs_mode_t mode);
    bool (*file_exists)(const char* path);
    bool (*create_file)(const char* path);
    bool (*create_dir)(const char* path);
    const char* (*get_error)(void);
    bool (*mounted)(void);
} fs_driver_t;

// VFS API
void vfs_init(void);
bool vfs_register(const char* name, fs_driver_t* driver);
bool vfs_mount(const char* fs_name, uint32_t partition_start);
void vfs_umount(void);

bool vfs_open(const char* path, vfs_file_t* file, vfs_mode_t mode);
bool vfs_read(vfs_file_t* file, void* buffer, uint32_t size, uint32_t* bytes_read);
bool vfs_write(vfs_file_t* file, const void* buffer, uint32_t size, uint32_t* bytes_written);
void vfs_close(vfs_file_t* file);
bool vfs_read_dir(vfs_file_t* file, vfs_dirent_t* entry);

bool vfs_file_exists(const char* path);
bool vfs_create_file(const char* path);
bool vfs_create_dir(const char* path);
const char* vfs_get_error(void);
bool vfs_mounted(void);

// 分区检测（自动查找 EXT2）
uint32_t vfs_detect_partition(void);

#endif
