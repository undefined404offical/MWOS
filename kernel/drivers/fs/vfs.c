#include "drivers/fs/vfs.h"
#include "drivers/fs/ext2.h"
#include "string.h"
#include "serial.h"
#include "memory.h"

#define VFS_MAX_DRIVERS 4

static fs_driver_t* g_drivers[VFS_MAX_DRIVERS];
static char g_driver_names[VFS_MAX_DRIVERS][16];
static int g_driver_count = 0;

static fs_driver_t* g_active_fs = NULL;
static char g_active_name[16] = {0};

static char vfs_error[64] = {0};

void vfs_init(void)
{
    memset(g_drivers, 0, sizeof(g_drivers));
    memset(g_driver_names, 0, sizeof(g_driver_names));
    g_driver_count = 0;
    g_active_fs = NULL;
    memset(vfs_error, 0, sizeof(vfs_error));

    serial_puts("VFS: Initialized\n");
}

bool vfs_register(const char* name, fs_driver_t* driver)
{
    if (!name || !driver) return false;
    if (g_driver_count >= VFS_MAX_DRIVERS) return false;

    strncpy(g_driver_names[g_driver_count], name, 15);
    g_driver_names[g_driver_count][15] = '\0';
    g_drivers[g_driver_count] = driver;
    g_driver_count++;

    serial_puts("VFS: Registered filesystem '");
    serial_puts(name);
    serial_puts("'\n");

    return true;
}

bool vfs_mount(const char* fs_name, uint32_t partition_start)
{
    if (!fs_name) {
        strcpy(vfs_error, "No filesystem name specified");
        return false;
    }

    for (int i = 0; i < g_driver_count; i++) {
        if (strcmp(g_driver_names[i], fs_name) == 0) {
            if (g_drivers[i]->mount(partition_start)) {
                g_active_fs = g_drivers[i];
                strncpy(g_active_name, fs_name, 15);
                g_active_name[15] = '\0';

                serial_puts("VFS: Mounted '");
                serial_puts(fs_name);
                serial_puts("' at partition LBA ");
                serial_putdec32(partition_start);
                serial_puts("\n");

                return true;
            } else {
                strcpy(vfs_error, g_drivers[i]->get_error());
                return false;
            }
        }
    }

    strcpy(vfs_error, "Filesystem driver not found");
    return false;
}

void vfs_umount(void)
{
    if (g_active_fs) {
        g_active_fs->umount();
        g_active_fs = NULL;
        memset(g_active_name, 0, sizeof(g_active_name));
    }
}

bool vfs_open(const char* path, vfs_file_t* file, vfs_mode_t mode)
{
    if (!g_active_fs) {
        strcpy(vfs_error, "No filesystem mounted");
        return false;
    }
    if (!path || !file) {
        strcpy(vfs_error, "Invalid parameters");
        return false;
    }

    return g_active_fs->open(path, file, mode);
}

bool vfs_read(vfs_file_t* file, void* buffer, uint32_t size, uint32_t* bytes_read)
{
    if (!file || !file->is_open) {
        strcpy(vfs_error, "File not open");
        return false;
    }
    if (file->read_fn) {
        return file->read_fn(file->fs_private, buffer, size, bytes_read);
    }
    strcpy(vfs_error, "Read not supported");
    return false;
}

bool vfs_write(vfs_file_t* file, const void* buffer, uint32_t size, uint32_t* bytes_written)
{
    if (!file || !file->is_open) {
        strcpy(vfs_error, "File not open");
        return false;
    }
    if (file->write_fn) {
        return file->write_fn(file->fs_private, buffer, size, bytes_written);
    }
    strcpy(vfs_error, "Write not supported");
    return false;
}

void vfs_close(vfs_file_t* file)
{
    if (file && file->is_open && file->close_fn) {
        file->close_fn(file->fs_private);
    }
    memset(file, 0, sizeof(vfs_file_t));
}

bool vfs_read_dir(vfs_file_t* file, vfs_dirent_t* entry)
{
    if (!file || !file->is_open || !file->read_dir_fn) {
        strcpy(vfs_error, "Directory read not supported");
        return false;
    }
    return file->read_dir_fn(file->fs_private, entry);
}

bool vfs_file_exists(const char* path)
{
    if (!g_active_fs) return false;
    return g_active_fs->file_exists(path);
}

bool vfs_create_file(const char* path)
{
    if (!g_active_fs) {
        strcpy(vfs_error, "No filesystem mounted");
        return false;
    }
    return g_active_fs->create_file(path);
}

bool vfs_create_dir(const char* path)
{
    if (!g_active_fs) {
        strcpy(vfs_error, "No filesystem mounted");
        return false;
    }
    return g_active_fs->create_dir(path);
}

const char* vfs_get_error(void)
{
    if (vfs_error[0]) return vfs_error;
    if (g_active_fs) return g_active_fs->get_error();
    return "Unknown error";
}

bool vfs_mounted(void)
{
    return g_active_fs != NULL && g_active_fs->mounted();
}

uint32_t vfs_detect_partition(void)
{
    // 委托给 ext2 的分区检测
    return ext2_detect();
}
