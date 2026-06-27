#include "drivers/fs/fat32.h"
#include "drivers/fs/fscache.h"
#include "drivers/disk.h"
#include "drivers/ide.h"
#include "serial.h"
#include "string.h"
#include "memory.h"
#include "io.h"
#include <stdbool.h>

static fat32_info_t fs_info;
static fat32_bpb_t bpb;
static bool fs_mounted = false;
static bool fs_readonly = false;
static uint32_t partition_start = 0;
static char error_msg[64] = {0};
static char volume_label[12] = {0};

static void clear_error(void);
static void set_error(const char* msg);
static uint32_t read_sector(uint32_t sector, void* buffer);
static uint32_t write_sector(uint32_t sector, const void* buffer);
static uint32_t cluster_to_sector(uint32_t cluster);
static uint32_t sector_to_cluster(uint32_t sector);
static uint32_t read_fat_entry(uint32_t cluster);
static bool write_fat_entry(uint32_t cluster, uint32_t value);
static uint32_t find_free_cluster(void);
static bool allocate_cluster_chain(uint32_t* cluster, uint32_t count);
static bool free_cluster_chain(uint32_t cluster);
static bool find_directory_entry(const char* path,
                                 uint32_t* dir_sector,
                                 uint32_t* dir_offset,
                                 uint32_t* parent_cluster);
static bool add_directory_entry(uint32_t parent_cluster,
                                const char* name83,
                                uint8_t attributes,
                                uint32_t first_cluster,
                                uint32_t file_size);
static bool update_directory_entry(fat32_handle_t* handle);
static bool remove_directory_entry(uint32_t dir_sector, uint32_t dir_offset);
static void name_to_83(const char* name, char* name83);
//static void format_83_name(const char* name83, char* name);
static bool is_valid_filename(const char* name);
static bool is_deleted_entry(const char* name);
static bool is_long_name_entry(const fat32_dir_entry_t* entry);
static uint32_t get_cluster_count(uint32_t first_cluster);
static void update_fs_info_sector(void);
static bool read_fs_info_sector(void);
static uint32_t find_next_cluster(uint32_t current_cluster,
                                  uint32_t position,
                                  uint32_t bytes_per_cluster);
static bool fat32_scan_free_clusters(void);
static bool fat32_low_level_format(uint32_t partition_start_sector,
                                   uint32_t partition_total_sectors,
                                   const char* volume_label_param);
uint32_t detect_fat32_partition(void);
static void build_83_name(const char* input, uint8_t out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';

    const char* dot = NULL;
    for (const char* p = input; *p; p++) {
        if (*p == '.') dot = p;
    }

    int name_len = 0;
    int ext_len = 0;

    if (dot) {
        name_len = dot - input;
        ext_len = strlen(dot + 1);
    } else {
        name_len = strlen(input);
        ext_len = 0;
    }

    if (name_len > 8) name_len = 8;
    if (ext_len > 3) ext_len = 3;

    // 填 name
    for (int i = 0; i < name_len; i++) {
        char c = input[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i] = c;
    }

    // 填 ext
    if (dot) {
        for (int i = 0; i < ext_len; i++) {
            char c = dot[1 + i];
            if (c >= 'a' && c <= 'z') c -= 32;
            out[8 + i] = c;
        }
    }
}

static const char* skip_slashes(const char* s) {
    while (*s == '/') s++;
    return s;
}

static const char* next_component(const char* path, char* out) {
    path = skip_slashes(path);
    if (*path == 0) {
        out[0] = 0;
        return path;
    }

    int i = 0;
    while (*path && *path != '/' && i < 63) {
        out[i++] = *path++;
    }
    out[i] = 0;
    return path;
}


void format_83_name(const char* src, char* dest) {
    int i, j = 0;

    for (i = 0; i < 8 && src[i] != ' '; i++) {
        dest[j++] = src[i];
    }

    if (src[8] != ' ') {
        dest[j++] = '.';

        for (i = 8; i < 11 && src[i] != ' '; i++) {
            dest[j++] = src[i];
        }
    }

    dest[j] = '\0';
}

static void clear_error(void) {
    error_msg[0] = '\0';
}

static void set_error(const char* msg) {
    int i = 0;
    while (msg[i] != '\0' && i < 63) {
        error_msg[i] = msg[i];
        i++;
    }
    error_msg[i] = '\0';
    serial_puts("FAT32 Error: ");
    serial_puts(msg);
    serial_puts("\n");
}

static uint32_t read_sector(uint32_t sector, void* buffer) {
    uint32_t physical_sector = sector + partition_start;
    
    // 使用文件系统缓存
    if (fscache_read_sector(0, physical_sector, buffer)) {
        return 0; // 成功
    }
    
    // 缓存失败，直接读取磁盘
    if (!disk_read(physical_sector, 1, buffer)) {
        return 0; // 成功
    }
    
    return 1; // 失败
}

static uint32_t write_sector(uint32_t sector, const void* buffer) {
    if (fs_readonly) {
        set_error("File system is read-only");
        return 1;
    }

    uint32_t physical_sector = sector + partition_start;
    
    // 使用文件系统缓存
    if (fscache_write_sector(0, physical_sector, buffer)) {
        return 0; // 成功
    }
    
    // 缓存失败，直接写入磁盘
    if (!disk_write(physical_sector, 1, buffer)) {
        return 0; // 成功
    }
    
    return 1; // 失败
}

bool fat32_path_lookup(const char* path, fat32_path_result_t* out) {
    clear_error();

    if (!fs_mounted || path == NULL || path[0] != '/') {
        set_error("Invalid path");
        return false;
    }

    uint32_t current_cluster = fs_info.root_dir_cluster;
    const char* p = path;
    char comp[64];

    p = skip_slashes(p);

    if (*p == 0) {
        memset(out, 0, sizeof(*out));
        out->dir_cluster = current_cluster;
        out->found = true;
        return true;
    }

    while (1) {
        p = next_component(p, comp);
        if (comp[0] == 0) break;

        uint8_t name83[11];
        build_83_name(comp, name83);

        bool found = false;
        uint32_t cluster = current_cluster;

        while (cluster < FAT32_LAST_CLUSTER) {
            uint32_t sector = cluster_to_sector(cluster);

            for (uint32_t s = 0; s < fs_info.sectors_per_cluster; s++, sector++) {
                uint8_t buf[512];
                read_sector(sector, buf);

                for (int i = 0; i < 512; i += 32) {
                    fat32_dir_entry_t* e = (fat32_dir_entry_t*)(buf + i);

                    if (e->name[0] == 0x00) goto scan_done;
                    if ((uint8_t)e->name[0] == 0xE5) continue;
                    if (is_long_name_entry(e)) continue;

                    if (memcmp(e->name, name83, 11) == 0) {
                        uint32_t fc = ((uint32_t)e->cluster_high << 16) | e->cluster_low;

                        const char* next = skip_slashes(p);
                        bool last = (*next == 0);

                        if (last) {
                            out->dir_cluster = current_cluster;
                            out->entry = *e;
                            out->entry_sector = sector;
                            out->entry_offset = i;
                            out->found = true;
                            return true;
                        }

                        if (!(e->attributes & ATTR_DIRECTORY)) {
                            set_error("Not a directory");
                            return false;
                        }

                        current_cluster = fc;
                        found = true;
                        goto next_comp;
                    }
                }
            }

            cluster = read_fat_entry(cluster);
        }

scan_done:
        if (!found) {
            out->dir_cluster = current_cluster;
            out->found = false;
            return true;
        }

next_comp:
        p = skip_slashes(p);
        if (*p == 0) break;
    }

    return true;
}


bool fat32_rename(const char* old_path, const char* new_path) {
    clear_error();

    fat32_path_result_t old_res;
    if (!fat32_path_lookup(old_path, &old_res) || !old_res.found) {
        set_error("Source not found");
        return false;
    }

    char new_dir_path[256];
    char new_name[64];

    const char* last = strrchr(new_path, '/');
    if (!last) {
        strcpy(new_dir_path, "/");
        strcpy(new_name, new_path);
    } else {
        size_t len = last - new_path;
        memcpy(new_dir_path, new_path, len);
        new_dir_path[len] = 0;
        strcpy(new_name, last + 1);
    }

    fat32_path_result_t new_dir_res;
    if (!fat32_path_lookup(new_dir_path, &new_dir_res) || !new_dir_res.found) {
        set_error("Target directory not found");
        return false;
    }

    uint8_t name83[11];
    build_83_name(new_name, name83);

    fat32_dir_entry_t* e = &old_res.entry;
    uint32_t first_cluster = ((uint32_t)e->cluster_high << 16) | e->cluster_low;

    if (!add_directory_entry(
            new_dir_res.dir_cluster,
            (const char*)name83,
            e->attributes,
            first_cluster,
            e->file_size)) {
        set_error("Failed to create new entry");
        return false;
    }

    uint8_t buf[512];
    read_sector(old_res.entry_sector, buf);
    buf[old_res.entry_offset] = 0xE5;
    write_sector(old_res.entry_sector, buf);

    return true;
}



/* ========= 簇 / 扇区 ↔ LBA ========= */

uint32_t cluster_to_sector(uint32_t cluster) {
    /* 簇号范围：2 ~ total_clusters+1 */
    if (cluster < 2 || cluster >= fs_info.total_clusters + 2) {
        return 0;
    }

    return fs_info.data_start_sector +
           (cluster - 2) * fs_info.sectors_per_cluster;
}

static uint32_t sector_to_cluster(uint32_t sector) {
    if (sector < fs_info.data_start_sector) {
        return 0;
    }
    return 2 + (sector - fs_info.data_start_sector) /
                fs_info.sectors_per_cluster;
}

/* ========= FAT 访问 ========= */

static uint32_t read_fat_entry(uint32_t cluster) {
    if (cluster < 2 || cluster >= fs_info.total_clusters + 2) {
        return 0x0FFFFFF7; /* 错误 */
    }

    uint32_t fat_offset = cluster * 4; /* FAT32 每项 4 字节 */
    uint32_t sector     = fs_info.fat_start_sector + (fat_offset / 512);
    uint32_t offset     = fat_offset % 512;

    static uint8_t f_buf[512] __attribute__((aligned(16)));
    if (read_sector(sector, f_buf) != 0) return 0x0FFFFFF7;

    uint32_t val = *(uint32_t*)(f_buf + offset);
    return val & 0x0FFFFFFF;
}

static bool write_fat_entry(uint32_t cluster, uint32_t value) {
    if (cluster < 2 || cluster >= fs_info.total_clusters + 2) {
        return false;
    }

    uint32_t fat_offset = cluster * 4;
    uint32_t sector     = fs_info.fat_start_sector + (fat_offset / 512);
    uint32_t offset     = fat_offset % 512;

    static uint8_t f_buf[512] __attribute__((aligned(16)));
    if (read_sector(sector, f_buf) != 0) return false;

    uint32_t old = *(uint32_t*)(f_buf + offset);
    uint32_t new_val = (old & 0xF0000000) | (value & 0x0FFFFFFF);
    *(uint32_t*)(f_buf + offset) = new_val;

    /* 写 FAT1 */
    if (write_sector(sector, f_buf) != 0) return false;

    /* 写 FAT2（位于 FAT1 后面） */
    uint32_t fat2_start = fs_info.fat_start_sector + fs_info.fat_sectors;
    uint32_t sector2    = fat2_start + (fat_offset / 512);
    if (write_sector(sector2, f_buf) != 0) return false;

    return true;
}

/* ========= 簇分配 / 释放 ========= */

uint32_t find_free_cluster(void) {
    /* 从簇 2 开始扫描到最后一个簇（簇号 = 2 + total_clusters - 1） */
    for (uint32_t c = 2; c < fs_info.total_clusters + 2; c++) {
        uint32_t entry = read_fat_entry(c);
        if (entry == FAT32_FREE_CLUSTER) {
            return c;
        }
    }
    return 0; /* 0 表示没找到 */
}

static bool allocate_cluster_chain(uint32_t* cluster, uint32_t count) {
    uint32_t first_cluster = 0;
    uint32_t prev_cluster  = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t new_cluster = find_free_cluster();
        if (new_cluster == 0) {
            set_error("Not enough free space");
            if (first_cluster != 0) {
                free_cluster_chain(first_cluster);
            }
            return false;
        }

        if (!write_fat_entry(new_cluster, FAT32_LAST_CLUSTER)) {
            if (first_cluster != 0) {
                free_cluster_chain(first_cluster);
            }
            return false;
        }

        if (i == 0) {
            first_cluster = new_cluster;
        } else {
            if (!write_fat_entry(prev_cluster, new_cluster)) {
                free_cluster_chain(first_cluster);
                return false;
            }
        }

        prev_cluster = new_cluster;
        if (fs_info.free_clusters > 0) {
            fs_info.free_clusters--;
        }
    }

    *cluster = first_cluster;
    return true;
}

static bool free_cluster_chain(uint32_t cluster) {
    while (cluster < FAT32_LAST_CLUSTER &&
           cluster != FAT32_FREE_CLUSTER &&
           cluster >= 2) {

        uint32_t next_cluster = read_fat_entry(cluster);
        if (!write_fat_entry(cluster, FAT32_FREE_CLUSTER)) {
            return false;
        }

        if (fs_info.free_clusters < fs_info.total_clusters) {
            fs_info.free_clusters++;
        }

        if (next_cluster >= FAT32_LAST_CLUSTER ||
            next_cluster == FAT32_FREE_CLUSTER ||
            next_cluster < 2) {
            break;
        }

        cluster = next_cluster;
    }

    return true;
}

/* ========= 8.3 文件名处理 ========= */

static void name_to_83(const char* name, char* name83) {
    for (int i = 0; i < 11; i++) {
        name83[i] = ' ';
    }

    const char* p = name;
    if (*p == '/') p++;

    const char* dot = strchr(p, '.');
    int name_len = 0;
    int ext_len = 0;

    if (dot == NULL) {
        name_len = strlen(p);
        if (name_len > 8) name_len = 8;
        for (int i = 0; i < name_len; i++) {
            name83[i] = toupper(p[i]);
        }
    } else {
        name_len = dot - p;
        if (name_len > 8) name_len = 8;
        for (int i = 0; i < name_len; i++) {
            name83[i] = toupper(p[i]);
        }

        const char* ext = dot + 1;
        ext_len = strlen(ext);
        if (ext_len > 3) ext_len = 3;
        for (int i = 0; i < ext_len; i++) {
            name83[8 + i] = toupper(ext[i]);
        }
    }
}

/*static void format_83_name(const char* name83, char* name) {
    int i = 0;

    while (i < 8 && name83[i] != ' ') {
        name[i] = name83[i];
        i++;
    }

    if (name83[8] != ' ') {
        name[i++] = '.';
        int j = 0;
        while (j < 3 && name83[8 + j] != ' ') {
            name[i++] = name83[8 + j];
            j++;
        }
    }

    name[i] = '\0';
}*/

static bool is_valid_filename(const char* name) {
    int len = strlen(name);
    if (len == 0 || len > 12) {
        return false;
    }

    const char* illegal_chars = "<>:\"/\\|?*";
    for (int i = 0; i < len; i++) {
        if (strchr(illegal_chars, name[i]) != NULL) {
            return false;
        }
    }

    return true;
}

static bool is_deleted_entry(const char* name) {
    return ((uint8_t)name[0]) == 0xE5;
}

static bool is_long_name_entry(const fat32_dir_entry_t* entry) {
    return (entry->attributes & ATTR_LONG_NAME) == ATTR_LONG_NAME;
}

/* ========= 目录项查找  ========= */

static bool find_directory_entry(const char* path,
                                 uint32_t* dir_sector,
                                 uint32_t* dir_offset,
                                 uint32_t* parent_cluster) {
    if (path == NULL || *path == '\0') {
        return false;
    }

    uint32_t current_cluster = fs_info.root_dir_cluster;
    uint32_t last_dir_cluster = current_cluster;

    const char* p = path;
    if (*p == '/') p++;

    while (*p) {
        char component[64];
        int len = 0;
        while (*p && *p != '/' && len < (int)sizeof(component) - 1) {
            component[len++] = *p++;
        }
        component[len] = '\0';

        if (len == 0) {
            if (*p == '/') p++;
            continue;
        }

        char name83[11];
        name_to_83(component, name83);

        bool found = false;

        // 在 current_cluster 对应的目录簇链中查找
        uint32_t search_cluster = current_cluster;
        while (search_cluster < FAT32_LAST_CLUSTER) {
            uint32_t sector = cluster_to_sector(search_cluster);

            for (uint32_t s = 0; s < fs_info.sectors_per_cluster; s++) {
                uint8_t sector_buffer[512];
                if (read_sector(sector + s, sector_buffer) != 0) {
                    return false;
                }

                for (int offset = 0; offset < 512; offset += 32) {
                    fat32_dir_entry_t* entry =
                        (fat32_dir_entry_t*)(sector_buffer + offset);

                    if (entry->name[0] == 0x00) {
                        goto search_done;
                    }

                    if (is_deleted_entry(entry->name) ||
                        is_long_name_entry(entry)) {
                        continue;
                    }

                    bool match = true;
                    for (int i = 0; i < 11; i++) {
                        if (entry->name[i] != name83[i]) {
                            match = false;
                            break;
                        }
                    }

                    if (match) {
                        last_dir_cluster = current_cluster;

                        if (*p == '/' || *(p) == '\0') {
                            if (*p == '/') {
                                if (!(entry->attributes & ATTR_DIRECTORY)) {
                                    return false;
                                }
                                current_cluster =
                                    (entry->cluster_high << 16) |
                                    entry->cluster_low;
                                found = true;
                                goto component_done;
                            } else {
                                if (dir_sector)  *dir_sector  = sector + s;
                                if (dir_offset)  *dir_offset  = offset;
                                if (parent_cluster) *parent_cluster = last_dir_cluster;
                                return true;
                            }
                        }
                    }
                }
            }

            uint32_t next = read_fat_entry(search_cluster);
            if (next == 0 || next >= FAT32_LAST_CLUSTER || next == 0x0FFFFFF7) {
                break;
            }
            search_cluster = next;
        }

    search_done:
        return false;

    component_done:
        if (*p == '/') p++; // 跳过 '/'
        continue;
    }

    return false;
}

/* ========= 目录项添加 ========= */

static bool add_directory_entry(uint32_t parent_cluster,
                                const char* name83,
                                uint8_t attributes,
                                uint32_t first_cluster,
                                uint32_t file_size) {
    uint32_t current_cluster = parent_cluster;

    while (current_cluster < FAT32_LAST_CLUSTER) {
        uint32_t sector = cluster_to_sector(current_cluster);

        for (uint32_t s = 0; s < fs_info.sectors_per_cluster; s++) {
            uint8_t sector_buffer[512];
            if (read_sector(sector + s, sector_buffer) != 0) {
                return false;
            }

            for (int offset = 0; offset < 512; offset += 32) {
                fat32_dir_entry_t* entry =
                    (fat32_dir_entry_t*)(sector_buffer + offset);

                if (entry->name[0] == 0x00 ||
                    is_deleted_entry(entry->name)) {

                    for (int i = 0; i < 11; i++) {
                        entry->name[i] = name83[i];
                    }

                    entry->attributes = attributes;
                    entry->reserved = 0;

                    entry->creation_time_tenths = 0;
                    entry->creation_time = 0;
                    entry->creation_date = 0;
                    entry->last_access_date = 0;

                    entry->cluster_high =
                        (first_cluster >> 16) & 0xFFFF;
                    entry->last_write_time = 0;
                    entry->last_write_date = 0;
                    entry->cluster_low = first_cluster & 0xFFFF;
                    entry->file_size = file_size;

                    if (write_sector(sector + s, sector_buffer) != 0) {
                        return false;
                    }

                    return true;
                }
            }
        }

        uint32_t next = read_fat_entry(current_cluster);
        if (next == 0 || next >= FAT32_LAST_CLUSTER ||
            next == 0x0FFFFFF7) {
            break;
        }
        current_cluster = next;
    }

    uint32_t new_cluster = find_free_cluster();
    if (new_cluster == 0) {
        return false;
    }

    if (!write_fat_entry(current_cluster, new_cluster)) {
        return false;
    }

    if (!write_fat_entry(new_cluster, FAT32_LAST_CLUSTER)) {
        return false;
    }

    uint32_t new_sector = cluster_to_sector(new_cluster);
    uint8_t empty_buffer[512] = {0};
    for (uint32_t s = 0; s < fs_info.sectors_per_cluster; s++) {
        if (write_sector(new_sector + s, empty_buffer) != 0) {
            return false;
        }
    }

    return add_directory_entry(new_cluster, name83,
                               attributes, first_cluster, file_size);
}

/* ========= 目录项更新 / 删除 ========= */

static bool update_directory_entry(fat32_handle_t* handle) {
    if (handle->dir_sector == 0) {
        return false;
    }

    uint8_t sector_buffer[512];
    if (read_sector(handle->dir_sector, sector_buffer) != 0) {
        return false;
    }

    fat32_dir_entry_t* entry =
        (fat32_dir_entry_t*)(sector_buffer + handle->dir_offset);

    entry->file_size = handle->file_size;
    entry->last_write_time = 0;
    entry->last_write_date = 0;

    return write_sector(handle->dir_sector, sector_buffer) == 0;
}

static bool remove_directory_entry(uint32_t dir_sector,
                                   uint32_t dir_offset) {
    uint8_t sector_buffer[512];
    if (read_sector(dir_sector, sector_buffer) != 0) {
        return false;
    }

    fat32_dir_entry_t* entry =
        (fat32_dir_entry_t*)(sector_buffer + dir_offset);
    entry->name[0] = 0xE5;

    return write_sector(dir_sector, sector_buffer) == 0;
}

/* ========= BPB / FSInfo 读取 & 挂载 ========= */

static fat32_bpb_t g_bpb;

/* 扫描 FAT 统计空闲簇数量 */
static bool fat32_scan_free_clusters(void) {
    if (fs_info.total_clusters == 0) {
        return false;
    }

    uint32_t free_count = 0;

    for (uint32_t c = 2; c < fs_info.total_clusters + 2; c++) {
        uint32_t val = read_fat_entry(c);
        if (val == FAT32_FREE_CLUSTER) {
            free_count++;
        } else if (val == 0x0FFFFFF7) {
            set_error("FAT scan failed: invalid FAT entry");
            return false;
        }
    }

    fs_info.free_clusters = free_count;
    return true;
}

bool fat32_init(uint32_t partition_start_sector) {
    partition_start = partition_start_sector;

    uint8_t sector_buffer[512] __attribute__((aligned(16)));

    /* 1. 读分区的 DBR（卷引导扇区） */
    if (read_sector(0, sector_buffer) != 0) {
        set_error("Failed to read FAT32 boot sector");
        return false;
    }
serial_puts("DBR bytes @0x0B..0x0F: ");
for (int i = 0x0B; i <= 0x0F; i++) {
    serial_puthex8(sector_buffer[i]);
    serial_puts(" ");
}
serial_puts("\n");

serial_puts("DBR bytes @0x2C..0x2F (RootClus raw): ");
for (int i = 0x2C; i <= 0x2F; i++) {
    serial_puthex8(sector_buffer[i]);
    serial_puts(" ");
}
serial_puts("\n");

    /* 2. 拷贝 BPB */
    memcpy(&g_bpb, sector_buffer, sizeof(fat32_bpb_t));
    bpb = g_bpb;
    serial_puts("INIT BPB decoded:\n");
serial_puts("  bytes_per_sector = ");
serial_putdec64(bpb.bytes_per_sector);
serial_puts("\n  sectors_per_cluster = ");
serial_putdec64(bpb.sectors_per_cluster);
serial_puts("\n  reserved_sectors = ");
serial_putdec64(bpb.reserved_sectors);
serial_puts("\n  fat_count = ");
serial_putdec64(bpb.fat_count);
serial_puts("\n  total_sectors_16 = ");
serial_putdec64(bpb.total_sectors_16);
serial_puts("\n  total_sectors_32 = ");
serial_putdec64(bpb.total_sectors_32);
serial_puts("\n  sectors_per_fat_16 = ");
serial_putdec64(bpb.sectors_per_fat_16);
serial_puts("\n  sectors_per_fat_32 = ");
serial_putdec64(bpb.sectors_per_fat_32);
serial_puts("\n  root_cluster = ");
serial_putdec64(bpb.root_cluster);
serial_puts("\n  fs_info_sector = ");
serial_putdec64(bpb.fs_info_sector);
serial_puts("\n  backup_boot_sector = ");
serial_putdec64(bpb.backup_boot_sector);
serial_puts("\n");

    /* 3. 从 BPB 解析关键字段 */
    fs_info.bytes_per_sector    = bpb.bytes_per_sector;
    fs_info.sectors_per_cluster = bpb.sectors_per_cluster;

    uint16_t res_sec   = bpb.reserved_sectors;
    uint8_t  fat_cnt   = bpb.fat_count;
    uint32_t tot_sec   = (bpb.total_sectors_16 != 0)
                         ? bpb.total_sectors_16
                         : bpb.total_sectors_32;
    uint32_t fat_sz    = (bpb.sectors_per_fat_16 != 0)
                         ? bpb.sectors_per_fat_16
                         : bpb.sectors_per_fat_32;
    uint32_t root_clus = bpb.root_cluster;
    uint16_t fsinfo_sec= bpb.fs_info_sector;

    if (fs_info.bytes_per_sector != 512 ||
        fs_info.sectors_per_cluster == 0 ||
        fat_cnt == 0 ||
        fat_sz == 0 ||
        root_clus < 2) {

        set_error("Invalid FAT32 BPB");
        serial_puts("FATAL: DBR Check failed. Root Cluster: ");
        serial_putdec64(root_clus);
        serial_puts("\n");
        return false;
    }

    /* 4. 计算 FAT / DATA 区的绝对 LBA */
    fs_info.fat_start_sector  = partition_start + res_sec;
    fs_info.fat_sectors       = fat_sz;
    fs_info.data_start_sector = partition_start + res_sec + fat_cnt * fat_sz;
    fs_info.root_dir_cluster  = root_clus;
    fs_info.total_sectors     = tot_sec;

    uint32_t data_sec = tot_sec - (res_sec + fat_cnt * fat_sz);
    fs_info.total_clusters = data_sec / fs_info.sectors_per_cluster;

    if (fs_info.total_clusters == 0) {
        set_error("No data clusters");
        return false;
    }

    /* 5. 尝试从 FSInfo 读取 free_clusters */
    bool fsinfo_ok = read_fs_info_sector();

    if (!fsinfo_ok ||
        fs_info.free_clusters == 0xFFFFFFFF ||
        fs_info.free_clusters > fs_info.total_clusters) {

        if (!fat32_scan_free_clusters()) {
            fs_info.free_clusters = fs_info.total_clusters;
        }

        /* 同步回 FSInfo 扇区 */
        update_fs_info_sector();
    }

    serial_puts("SUCCESS: Root cluster = ");
    serial_putdec64(fs_info.root_dir_cluster);
    serial_puts("\n");

    fs_mounted = true;
    return true;
}

bool fat32_mount(uint32_t partition_start_sector) {
    return fat32_init(partition_start_sector);
}

void fat32_umount(void) {
    fs_mounted = false;
    memset(&fs_info, 0, sizeof(fs_info));
    memset(&bpb, 0, sizeof(bpb));
    volume_label[0] = '\0';
}

/* ========= 文件打开 / 创建 ========= */

bool fat32_open(const char* path, fat32_handle_t* handle, file_mode_t mode) {
    clear_error();

    if (!fs_mounted) {
        set_error("File system not mounted");
        return false;
    }

    if (path == NULL || handle == NULL) {
        set_error("Invalid parameters");
        return false;
    }
    handle->current_cluster = handle->first_cluster;
handle->buffer_sector = 0xFFFFFFFF;  // 强制第一次读 sector
handle->position = 0;

    memset(handle, 0, sizeof(fat32_handle_t));

    if (strcmp(path, "/") == 0 || strcmp(path, "") == 0) {
        if (mode != FILE_READ) {
            set_error("Cannot write to root directory");
            return false;
        }

        handle->first_cluster   = fs_info.root_dir_cluster;
        handle->current_cluster = fs_info.root_dir_cluster;
        handle->is_directory    = true;
        handle->is_open         = true;
        return true;
    }

    uint32_t dir_sector = 0, dir_offset = 0, parent_cluster = 0;
    if (find_directory_entry(path, &dir_sector, &dir_offset, &parent_cluster)) {

        uint8_t sector_buffer[512];
        if (read_sector(dir_sector, sector_buffer) != 0) {
            return false;
        }

        fat32_dir_entry_t* entry =
            (fat32_dir_entry_t*)(sector_buffer + dir_offset);

        handle->first_cluster =
            (entry->cluster_high << 16) | entry->cluster_low;
        handle->current_cluster = handle->first_cluster;
        handle->file_size       = entry->file_size;
        handle->is_directory    = (entry->attributes & ATTR_DIRECTORY) != 0;
        handle->dir_sector      = dir_sector;
        handle->dir_offset      = dir_offset;
        handle->is_open         = true;

        if (mode == FILE_READ && (entry->attributes & ATTR_READ_ONLY)) {
            set_error("File is read-only");
            return false;
        }

        return true;
    }

    if (mode == FILE_CREATE || mode == FILE_APPEND) {

        if (!fat32_create_file(path)) {
            return false;
        }

        if (!find_directory_entry(path, &dir_sector, &dir_offset, &parent_cluster)) {
            set_error("Failed to locate newly created file");
            return false;
        }

        uint8_t sector_buffer[512];
        if (read_sector(dir_sector, sector_buffer) != 0) {
            return false;
        }

        fat32_dir_entry_t* entry =
            (fat32_dir_entry_t*)(sector_buffer + dir_offset);

        handle->first_cluster =
            (entry->cluster_high << 16) | entry->cluster_low;
        handle->current_cluster = handle->first_cluster;
        handle->file_size       = entry->file_size;
        handle->is_directory    = false;
        handle->dir_sector      = dir_sector;
        handle->dir_offset      = dir_offset;
        handle->is_open         = true;

        return true;
    }

    set_error("File not found");
    return false;
}

bool fat32_open_root(fat32_handle_t* handle) {
    return fat32_open("/", handle, FILE_READ);
}

/* ========= 文件读写 ========= */
bool fat32_read(fat32_handle_t* handle, void* buffer, uint32_t size) {
    clear_error();

    if (!fs_mounted || !handle || !buffer || !handle->is_open)
        return false;

    if (handle->is_directory)
        return false;

    if (handle->position >= handle->file_size)
        return false;

    if (handle->position + size > handle->file_size)
        size = handle->file_size - handle->position;

    uint8_t* dest = (uint8_t*)buffer;
    uint32_t bytes_read = 0;

    uint32_t bytes_per_cluster =
        fs_info.sectors_per_cluster * fs_info.bytes_per_sector;

    uint32_t pos = handle->position;

    // 定位当前 cluster
    uint32_t cluster = handle->current_cluster;
    if (cluster == 0) {
        uint32_t cluster_index = pos / bytes_per_cluster;
        cluster = handle->first_cluster;
        for (uint32_t i = 0; i < cluster_index; i++) {
            cluster = read_fat_entry(cluster);
            if (cluster >= FAT32_LAST_CLUSTER)
                return false;
        }
        handle->current_cluster = cluster;
    }

    // 最大支持 64KB cluster（4K * 16），你现在是 4*512=2KB，绰绰有余
    static uint8_t cluster_buf[64 * 1024];

    while (bytes_read < size) {
        uint32_t cluster_offset = pos % bytes_per_cluster;
        uint32_t cluster_lba    = cluster_to_sector(cluster);
        uint32_t cluster_bytes  = bytes_per_cluster;

        uint32_t remain = size - bytes_read;
        uint32_t can_read = cluster_bytes - cluster_offset;
        if (can_read > remain) can_read = remain;

        /*serial_puts("[FAT32] cluster=");
        serial_putdec64(cluster);
        serial_puts(" pos=");
        serial_putdec64(pos);
        serial_puts(" offs=");
        serial_putdec64(cluster_offset);
        serial_puts(" can_read=");
        serial_putdec64(can_read);
        serial_puts("\n");*/

        // 先把整个 cluster 读到临时缓冲区
        if (!ide_read_auto(cluster_lba, cluster_bytes, cluster_buf)) {
            set_error("multi-sector read failed");
            return false;
        }

        memcpy(dest + bytes_read,
               cluster_buf + cluster_offset,
               can_read);

        bytes_read += can_read;
        pos        += can_read;

        if (pos % bytes_per_cluster == 0 && bytes_read < size) {
            cluster = read_fat_entry(cluster);
            if (cluster >= FAT32_LAST_CLUSTER)
                break;
            handle->current_cluster = cluster;
        }
    }

    handle->position = pos;
    return bytes_read > 0;
}

bool fat32_write(fat32_handle_t* handle,
                 const void* buffer,
                 uint32_t size) {
    clear_error();

    if (!fs_mounted || handle == NULL || buffer == NULL ||
        !handle->is_open || fs_readonly) {
        set_error("Invalid parameters or read-only");
        return false;
    }

    if (handle->is_directory) {
        set_error("Cannot write to directory");
        return false;
    }

    const uint8_t* src = (const uint8_t*)buffer;
    uint32_t bytes_written = 0;

    while (bytes_written < size) {
        uint32_t bytes_per_cluster =
            fs_info.sectors_per_cluster *
            fs_info.bytes_per_sector;
        uint32_t cluster_offset =
            handle->position % bytes_per_cluster;
        uint32_t sector_in_cluster =
            cluster_offset / fs_info.bytes_per_sector;
        uint32_t sector_offset =
            cluster_offset % fs_info.bytes_per_sector;

        if (cluster_offset == 0 && handle->position > 0) {
            uint32_t next_cluster =
                read_fat_entry(handle->current_cluster);
            if (next_cluster >= FAT32_LAST_CLUSTER ||
                next_cluster == FAT32_FREE_CLUSTER) {

                uint32_t new_cluster = find_free_cluster();
                if (new_cluster == 0) {
                    set_error("No free space");
                    return false;
                }

                if (!write_fat_entry(handle->current_cluster,
                                     new_cluster) ||
                    !write_fat_entry(new_cluster,
                                     FAT32_LAST_CLUSTER)) {
                    set_error("Failed to allocate cluster");
                    return false;
                }

                handle->current_cluster = new_cluster;
                if (fs_info.free_clusters > 0) {
                    fs_info.free_clusters--;
                }
            } else {
                handle->current_cluster = next_cluster;
            }
        }

        uint32_t sector =
            cluster_to_sector(handle->current_cluster) +
            sector_in_cluster;
        if (sector != handle->buffer_sector) {
            if (handle->buffer_dirty &&
                handle->buffer_sector != 0) {
                if (write_sector(handle->buffer_sector,
                                 handle->buffer) != 0) {
                    set_error("Failed to write sector");
                    return false;
                }
                handle->buffer_dirty = false;
            }

            if (handle->position < handle->file_size) {
                if (read_sector(sector,
                                handle->buffer) != 0) {
                    set_error("Failed to read sector");
                    return false;
                }
            } else {
                memset(handle->buffer, 0,
                       fs_info.bytes_per_sector);
            }

            handle->buffer_sector = sector;
        }

        uint32_t bytes_to_write =
            fs_info.bytes_per_sector - sector_offset;
        if (bytes_to_write > size - bytes_written) {
            bytes_to_write = size - bytes_written;
        }

        memcpy(handle->buffer + sector_offset,
               src + bytes_written,
               bytes_to_write);
        handle->buffer_dirty = true;

        bytes_written   += bytes_to_write;
        handle->position += bytes_to_write;

        if (handle->position > handle->file_size) {
            handle->file_size = handle->position;
        }
    }

    return true;
}

/* ========= seek / truncate / close ========= */

bool fat32_seek(fat32_handle_t* handle, uint32_t position) {
    clear_error();

    if (!fs_mounted || handle == NULL || !handle->is_open) {
        set_error("Invalid parameters");
        return false;
    }

    if (position > handle->file_size &&
        !handle->is_directory) {
        set_error("Seek beyond end of file");
        return false;
    }

    if (position == 0) {
        handle->current_cluster = handle->first_cluster;
        handle->position        = 0;
        handle->buffer_sector   = 0;
        handle->buffer_dirty    = false;
        return true;
    }

    uint32_t bytes_per_cluster =
        fs_info.sectors_per_cluster *
        fs_info.bytes_per_sector;
    uint32_t target_cluster_offset =
        position / bytes_per_cluster;

    uint32_t current_cluster = handle->first_cluster;
    for (uint32_t i = 0; i < target_cluster_offset; i++) {
        uint32_t next_cluster = read_fat_entry(current_cluster);
        if (next_cluster >= FAT32_LAST_CLUSTER ||
            next_cluster == FAT32_FREE_CLUSTER) {
            set_error("Invalid cluster chain");
            return false;
        }
        current_cluster = next_cluster;
    }

    handle->current_cluster = current_cluster;
    handle->position        = position;
    handle->buffer_sector   = 0;
    handle->buffer_dirty    = false;

    return true;
}

bool fat32_truncate(fat32_handle_t* handle,
                    uint32_t new_size) {
    clear_error();

    if (!fs_mounted || handle == NULL || !handle->is_open ||
        fs_readonly) {
        set_error("Invalid parameters or read-only");
        return false;
    }

    if (handle->is_directory) {
        set_error("Cannot truncate directory");
        return false;
    }

    if (new_size == handle->file_size) {
        return true;
    }

    uint32_t bytes_per_cluster =
        fs_info.sectors_per_cluster *
        fs_info.bytes_per_sector;

    if (new_size > handle->file_size) {
        handle->file_size = new_size;
        return update_directory_entry(handle);
    } else {
        uint32_t old_cluster_count =
            (handle->file_size + bytes_per_cluster - 1) /
            bytes_per_cluster;
        uint32_t new_cluster_count =
            (new_size + bytes_per_cluster - 1) /
            bytes_per_cluster;

        if (new_cluster_count < old_cluster_count) {
            uint32_t current_cluster = handle->first_cluster;
            uint32_t prev_cluster    = 0;

            for (uint32_t i = 0; i < new_cluster_count; i++) {
                prev_cluster = current_cluster;
                current_cluster = read_fat_entry(current_cluster);
            }

            if (prev_cluster != 0) {
                uint32_t first_to_free = current_cluster;

                if (!write_fat_entry(prev_cluster,
                                     FAT32_LAST_CLUSTER)) {
                    return false;
                }

                if (first_to_free < FAT32_LAST_CLUSTER &&
                    first_to_free != FAT32_FREE_CLUSTER) {
                    if (!free_cluster_chain(first_to_free)) {
                        return false;
                    }
                }
            }
        }

        handle->file_size = new_size;

        if (handle->position > new_size) {
            handle->position = new_size;
        }

        return update_directory_entry(handle);
    }
}

void fat32_close(fat32_handle_t* handle) {
    if (handle == NULL || !handle->is_open) {
        return;
    }

    if (handle->buffer_dirty &&
        handle->buffer_sector != 0) {
        write_sector(handle->buffer_sector,
                     handle->buffer);
    }

    if (handle->dir_sector != 0) {
        update_directory_entry(handle);
    }

    memset(handle, 0, sizeof(fat32_handle_t));
}

/* ========= 目录操作 ========= */

bool fat32_create_dir(const char* path) {
    clear_error();

    if (!fs_mounted || fs_readonly) {
        set_error("File system not mounted or read-only");
        return false;
    }

    if (path == NULL || strlen(path) == 0) {
        set_error("Invalid path");
        return false;
    }

    uint32_t dir_sector, dir_offset, parent_cluster;
    if (find_directory_entry(path, &dir_sector, &dir_offset, &parent_cluster)) {
        set_error("Directory already exists");
        return false;
    }

    const char* p = path;
    if (*p == '/') p++;

    const char* last_slash = strrchr(p, '/');
    uint32_t parent_clus = fs_info.root_dir_cluster;
    char name_component[64];

    if (last_slash) {
        // 父路径部分 [p, last_slash)
        int plen = last_slash - p;
        if (plen <= 0 || plen >= (int)sizeof(name_component)) {
            set_error("Invalid path");
            return false;
        }
        char parent_path[128];
        memcpy(parent_path, p, plen);
        parent_path[plen] = '\0';

        uint32_t p_sec, p_off, p_parent;
        if (!find_directory_entry(parent_path, &p_sec, &p_off, &p_parent)) {
            set_error("Parent directory not found");
            return false;
        }

        uint8_t sector_buffer[512];
        if (read_sector(p_sec, sector_buffer) != 0) {
            return false;
        }

        fat32_dir_entry_t* parent_entry =
            (fat32_dir_entry_t*)(sector_buffer + p_off);
        if (!(parent_entry->attributes & ATTR_DIRECTORY)) {
            set_error("Parent is not a directory");
            return false;
        }

        parent_clus =
            (parent_entry->cluster_high << 16) |
            parent_entry->cluster_low;

        const char* comp = last_slash + 1;
        strncpy(name_component, comp, sizeof(name_component) - 1);
        name_component[sizeof(name_component) - 1] = '\0';
    } else {
        // 没有父路径，如 "/TEST_DIR"
        parent_clus = fs_info.root_dir_cluster;
        strncpy(name_component, p, sizeof(name_component) - 1);
        name_component[sizeof(name_component) - 1] = '\0';
    }

    uint32_t first_cluster = 0;
    if (!allocate_cluster_chain(&first_cluster, 1)) {
        return false;
    }

    char name83[11];
    name_to_83(name_component, name83);

    if (!add_directory_entry(parent_clus,
                             name83,
                             ATTR_DIRECTORY,
                             first_cluster,
                             0)) {
        free_cluster_chain(first_cluster);
        return false;
    }

    uint8_t sector_buffer[512];
    memset(sector_buffer, 0, sizeof(sector_buffer));

    fat32_dir_entry_t* dot_entry =
        (fat32_dir_entry_t*)sector_buffer;
    dot_entry->name[0] = '.';
    for (int i = 1; i < 11; i++) dot_entry->name[i] = ' ';
    dot_entry->attributes   = ATTR_DIRECTORY;
    dot_entry->cluster_high = (first_cluster >> 16) & 0xFFFF;
    dot_entry->cluster_low  = first_cluster & 0xFFFF;
    dot_entry->file_size    = 0;

    fat32_dir_entry_t* dotdot_entry =
        (fat32_dir_entry_t*)(sector_buffer + 32);
    dotdot_entry->name[0] = '.';
    dotdot_entry->name[1] = '.';
    for (int i = 2; i < 11; i++)
        dotdot_entry->name[i] = ' ';
    dotdot_entry->attributes = ATTR_DIRECTORY;
    dotdot_entry->cluster_high = (parent_clus >> 16) & 0xFFFF;
    dotdot_entry->cluster_low  = parent_clus & 0xFFFF;
    dotdot_entry->file_size    = 0;

    uint32_t dir_sector_start = cluster_to_sector(first_cluster);
    if (write_sector(dir_sector_start, sector_buffer) != 0) {
        return false;
    }

    return true;
}

bool fat32_remove_dir(const char* path) {
    clear_error();

    if (!fs_mounted || fs_readonly) {
        set_error("File system not mounted or read-only");
        return false;
    }

    uint32_t dir_sector, dir_offset, parent_cluster;
    if (!find_directory_entry(path, &dir_sector,
                              &dir_offset,
                              &parent_cluster)) {
        set_error("Directory not found");
        return false;
    }

    uint8_t sector_buffer[512];
    if (read_sector(dir_sector, sector_buffer) != 0) {
        return false;
    }

    fat32_dir_entry_t* entry =
        (fat32_dir_entry_t*)(sector_buffer + dir_offset);

    if ((entry->attributes & ATTR_DIRECTORY) == 0) {
        set_error("Not a directory");
        return false;
    }

    uint32_t dir_cluster =
        (entry->cluster_high << 16) | entry->cluster_low;

    if (dir_cluster != 0) {
        if (!free_cluster_chain(dir_cluster)) {
            return false;
        }
    }

    return remove_directory_entry(dir_sector, dir_offset);
}

bool fat32_read_dir(fat32_handle_t* dir_handle,
                    fat32_dir_entry_t* entry) {
    clear_error();

    if (!fs_mounted || dir_handle == NULL ||
        entry == NULL || !dir_handle->is_open) {
        set_error("Invalid parameters");
        return false;
    }

    if (!dir_handle->is_directory) {
        set_error("Not a directory handle");
        return false;
    }

    if (dir_handle->current_cluster >= FAT32_LAST_CLUSTER) {
        return false;
    }

    uint8_t sector_buffer[512];

    while (dir_handle->current_cluster < FAT32_LAST_CLUSTER) {
        uint32_t sector =
            cluster_to_sector(dir_handle->current_cluster);
        uint32_t sector_in_cluster =
            dir_handle->position / fs_info.bytes_per_sector;
        uint32_t sector_offset =
            dir_handle->position % fs_info.bytes_per_sector;

        if (sector_in_cluster >= fs_info.sectors_per_cluster) {
            dir_handle->current_cluster =
                read_fat_entry(dir_handle->current_cluster);
            if (dir_handle->current_cluster >=
                FAT32_LAST_CLUSTER) {
                return false;
            }
            dir_handle->position = 0;
            sector_in_cluster    = 0;
            sector_offset        = 0;
        }

        if (read_sector(sector + sector_in_cluster,
                        sector_buffer) != 0) {
            set_error("Failed to read directory sector");
            return false;
        }

        for (; sector_offset < fs_info.bytes_per_sector;
             sector_offset += 32) {

            fat32_dir_entry_t* dir_entry =
                (fat32_dir_entry_t*)(sector_buffer +
                                     sector_offset);

            if (dir_entry->name[0] == 0x00) {
                return false;
            }

            if (is_deleted_entry(dir_entry->name) ||
                is_long_name_entry(dir_entry)) {
                continue;
            }

            if (dir_entry->name[0] == '.' &&
                (dir_entry->name[1] == ' ' ||
                 dir_entry->name[1] == '.')) {
                continue;
            }

            memcpy(entry, dir_entry,
                   sizeof(fat32_dir_entry_t));

            dir_handle->position =
                (sector_in_cluster *
                 fs_info.bytes_per_sector) +
                sector_offset + 32;

            return true;
        }

        dir_handle->position =
            (sector_in_cluster + 1) *
            fs_info.bytes_per_sector;
    }

    return false;
}

/* ========= 文件操作封装 ========= */

bool fat32_find_file(const char* path,
                     fat32_dir_entry_t* entry) {
    clear_error();

    if (!fs_mounted || path == NULL || entry == NULL) {
        set_error("Invalid parameters");
        return false;
    }

    uint32_t dir_sector, dir_offset, parent_cluster;
    if (!find_directory_entry(path, &dir_sector,
                              &dir_offset,
                              &parent_cluster)) {
        return false;
    }

    uint8_t sector_buffer[512];
    if (read_sector(dir_sector, sector_buffer) != 0) {
        return false;
    }

    memcpy(entry, sector_buffer + dir_offset,
           sizeof(fat32_dir_entry_t));
    return true;
}

bool fat32_create_file(const char* path) {
    clear_error();

    if (!fs_mounted || fs_readonly) {
        set_error("File system not mounted or read-only");
        return false;
    }

    if (path == NULL || strlen(path) == 0) {
        set_error("Invalid path");
        return false;
    }

    uint32_t dir_sector, dir_offset, parent_cluster;
    if (find_directory_entry(path, &dir_sector, &dir_offset, &parent_cluster)) {
        set_error("File already exists");
        return false;
    }

    const char* p = path;
    if (*p == '/') p++;

    const char* last_slash = strrchr(p, '/');
    uint32_t parent_clus = fs_info.root_dir_cluster;
    char name_component[64];

    if (last_slash) {
        int plen = last_slash - p;
        if (plen <= 0 || plen >= (int)sizeof(name_component)) {
            set_error("Invalid path");
            return false;
        }
        char parent_path[128];
        memcpy(parent_path, p, plen);
        parent_path[plen] = '\0';

        uint32_t p_sec, p_off, p_parent;
        if (!find_directory_entry(parent_path, &p_sec, &p_off, &p_parent)) {
            set_error("Parent directory not found");
            return false;
        }

        uint8_t sector_buffer[512];
        if (read_sector(p_sec, sector_buffer) != 0) {
            return false;
        }

        fat32_dir_entry_t* parent_entry =
            (fat32_dir_entry_t*)(sector_buffer + p_off);
        if (!(parent_entry->attributes & ATTR_DIRECTORY)) {
            set_error("Parent is not a directory");
            return false;
        }

        parent_clus =
            (parent_entry->cluster_high << 16) |
            parent_entry->cluster_low;

        const char* comp = last_slash + 1;
        strncpy(name_component, comp, sizeof(name_component) - 1);
        name_component[sizeof(name_component) - 1] = '\0';
    } else {
        parent_clus = fs_info.root_dir_cluster;
        strncpy(name_component, p, sizeof(name_component) - 1);
        name_component[sizeof(name_component) - 1] = '\0';
    }

    uint32_t first_cluster = 0;
    if (!allocate_cluster_chain(&first_cluster, 1)) {
        return false;
    }

    char name83[11];
    name_to_83(name_component, name83);

    if (!add_directory_entry(parent_clus,
                             name83,
                             ATTR_ARCHIVE,
                             first_cluster,
                             0)) {
        free_cluster_chain(first_cluster);
        return false;
    }

    uint32_t sector = cluster_to_sector(first_cluster);
    uint8_t zero_buffer[512] = {0};
    for (uint32_t i = 0; i < fs_info.sectors_per_cluster; i++) {
        if (write_sector(sector + i, zero_buffer) != 0) {
            return false;
        }
    }

    return true;
}

bool fat32_delete_file(const char* path) {
    clear_error();

    if (!fs_mounted || fs_readonly) {
        set_error("File system not mounted or read-only");
        return false;
    }

    uint32_t dir_sector, dir_offset, parent_cluster;
    if (!find_directory_entry(path, &dir_sector,
                              &dir_offset,
                              &parent_cluster)) {
        set_error("File not found");
        return false;
    }

    uint8_t sector_buffer[512];
    if (read_sector(dir_sector, sector_buffer) != 0) {
        return false;
    }

    fat32_dir_entry_t* entry =
        (fat32_dir_entry_t*)(sector_buffer + dir_offset);

    if ((entry->attributes & ATTR_DIRECTORY) != 0) {
        set_error("Is a directory, use remove_dir instead");
        return false;
    }

    uint32_t file_cluster =
        (entry->cluster_high << 16) | entry->cluster_low;

    if (file_cluster != 0) {
        if (!free_cluster_chain(file_cluster)) {
            return false;
        }
    }

    return remove_directory_entry(dir_sector, dir_offset);
}

/* 简化版 copy */
bool fat32_copy(const char* src_path, const char* dst_path) {
    clear_error();

    if (!fs_mounted || fs_readonly) {
        set_error("File system not mounted or read-only");
        return false;
    }

    fat32_handle_t src_handle;
    if (!fat32_open(src_path, &src_handle, FILE_READ)) {
        return false;
    }

    uint32_t dir_sector, dir_offset, parent_cluster;
    if (find_directory_entry(dst_path, &dir_sector,
                             &dir_offset,
                             &parent_cluster)) {
        set_error("Destination file already exists");
        fat32_close(&src_handle);
        return false;
    }

    fat32_handle_t dst_handle;
    if (!fat32_open(dst_path, &dst_handle, FILE_CREATE)) {
        fat32_close(&src_handle);
        return false;
    }

    uint8_t buffer[512];

    while (true) {
        if (!fat32_read(&src_handle, buffer, sizeof(buffer))) {
            break;
        }

        if (!fat32_write(&dst_handle, buffer, sizeof(buffer))) {
            fat32_close(&src_handle);
            fat32_close(&dst_handle);
            return false;
        }
    }

    fat32_close(&src_handle);
    fat32_close(&dst_handle);

    return true;
}

bool fat32_move(const char* src_path, const char* dst_path) {
    if (fat32_rename(src_path, dst_path)) {
        return true;
    }

    if (fat32_copy(src_path, dst_path)) {
        return fat32_delete_file(src_path);
    }

    return false;
}

bool fat32_file_exists(const char* path) {
    clear_error();

    if (!fs_mounted || path == NULL) {
        return false;
    }

    uint32_t dir_sector, dir_offset, parent_cluster;
    return find_directory_entry(path, &dir_sector,
                                &dir_offset,
                                &parent_cluster);
}

uint32_t fat32_get_file_size(const char* path) {
    clear_error();

    if (!fs_mounted || path == NULL) {
        return 0;
    }

    uint32_t dir_sector, dir_offset, parent_cluster;
    if (!find_directory_entry(path, &dir_sector,
                              &dir_offset,
                              &parent_cluster)) {
        return 0;
    }

    uint8_t sector_buffer[512];
    if (read_sector(dir_sector, sector_buffer) != 0) {
        return 0;
    }

    fat32_dir_entry_t* entry =
        (fat32_dir_entry_t*)(sector_buffer + dir_offset);
    return entry->file_size;
}

bool fat32_get_file_info(const char* path,
                         fat32_dir_entry_t* info) {
    clear_error();

    if (!fs_mounted || path == NULL || info == NULL) {
        return false;
    }

    uint32_t dir_sector, dir_offset, parent_cluster;
    if (!find_directory_entry(path, &dir_sector,
                              &dir_offset,
                              &parent_cluster)) {
        return false;
    }

    uint8_t sector_buffer[512];
    if (read_sector(dir_sector, sector_buffer) != 0) {
        return false;
    }

    memcpy(info, sector_buffer + dir_offset,
           sizeof(fat32_dir_entry_t));
    return true;
}

bool fat32_set_file_attributes(const char* path,
                               uint8_t attributes) {
    clear_error();

    if (!fs_mounted || fs_readonly || path == NULL) {
        set_error("Invalid parameters or read-only");
        return false;
    }

    uint32_t dir_sector, dir_offset, parent_cluster;
    if (!find_directory_entry(path, &dir_sector,
                              &dir_offset,
                              &parent_cluster)) {
        set_error("File not found");
        return false;
    }

    uint8_t sector_buffer[512];
    if (read_sector(dir_sector, sector_buffer) != 0) {
        return false;
    }

    fat32_dir_entry_t* entry =
        (fat32_dir_entry_t*)(sector_buffer + dir_offset);
    entry->attributes = attributes;

    return write_sector(dir_sector, sector_buffer) == 0;
}

/* ========= 信息 / 工具 ========= */

const char* fat32_get_error(void) {
    return error_msg;
}

bool fat32_mounted(void) {
    return fs_mounted;
}

bool fat32_format_check(void) {
    clear_error();

    if (!fs_mounted) {
        set_error("File system not mounted");
        return false;
    }

    uint8_t boot_sector[512];
    if (read_sector(partition_start, boot_sector) != 0) {
        set_error("Failed to read boot sector");
        return false;
    }

    if (boot_sector[510] != 0x55 ||
        boot_sector[511] != 0xAA) {
        set_error("Invalid boot signature");
        return false;
    }

    uint32_t test_clusters[] =
        {0, 1, 2, fs_info.total_clusters + 1};

    for (int i = 0; i < 4; i++) {
        uint32_t fat_entry = read_fat_entry(test_clusters[i]);
        if (fat_entry == 0xFFFFFFFF) {
            set_error("FAT table corrupted");
            return false;
        }
    }

    return true;
}

void fat32_print_info(void) {
    if (!fs_mounted) {
        serial_puts("FAT32 file system not mounted\n");
        return;
    }

    serial_puts("FAT32 File System Information:\n");
    serial_puts("==============================\n");

    serial_puts("Volume label: ");
    serial_puts(volume_label);
    serial_puts("\n");

    serial_puts("Bytes per sector: ");
    serial_putdec64(fs_info.bytes_per_sector);
    serial_puts("\n");

    serial_puts("Sectors per cluster: ");
    serial_putdec64(fs_info.sectors_per_cluster);
    serial_puts("\n");

    serial_puts("Total sectors: ");
    serial_putdec64(fs_info.total_sectors);
    serial_puts("\n");

    serial_puts("Total clusters: ");
    serial_putdec64(fs_info.total_clusters);
    serial_puts("\n");

    serial_puts("Free clusters: ");
    serial_putdec64(fs_info.free_clusters);
    serial_puts("\n");

    serial_puts("Data start sector: ");
    serial_putdec64(fs_info.data_start_sector);
    serial_puts("\n");

    serial_puts("Root directory cluster: ");
    serial_putdec64(fs_info.root_dir_cluster);
    serial_puts("\n");

    uint32_t total_bytes =
        fs_info.total_sectors * fs_info.bytes_per_sector;
    uint32_t free_bytes =
        fs_info.free_clusters *
        fs_info.sectors_per_cluster *
        fs_info.bytes_per_sector;
    uint32_t used_bytes = total_bytes - free_bytes;

    serial_puts("Total space: ");
    serial_putdec64(total_bytes / 1024);
    serial_puts(" KB\n");

    serial_puts("Used space: ");
    serial_putdec64(used_bytes / 1024);
    serial_puts(" KB (");
    serial_putdec64(
        (used_bytes * 100) / total_bytes);
    serial_puts("%)\n");

    serial_puts("Free space: ");
    serial_putdec64(free_bytes / 1024);
    serial_puts(" KB (");
    serial_putdec64(
        (free_bytes * 100) / total_bytes);
    serial_puts("%)\n");

    serial_puts("==============================\n");
}

uint32_t fat32_get_free_space(void) {
    if (!fs_mounted) {
        return 0;
    }

    return fs_info.free_clusters *
           fs_info.sectors_per_cluster *
           fs_info.bytes_per_sector;
}

uint32_t fat32_get_total_space(void) {
    if (!fs_mounted) {
        return 0;
    }

    return fs_info.total_sectors *
           fs_info.bytes_per_sector;
}

const char* fat32_get_volume_label(void) {
    return volume_label;
}

bool fat32_set_volume_label(const char* label) {
    clear_error();

    if (!fs_mounted || fs_readonly) {
        set_error("File system not mounted or read-only");
        return false;
    }

    if (label == NULL || strlen(label) > 11) {
        set_error("Invalid volume label");
        return false;
    }

    memset(volume_label, 0, sizeof(volume_label));
    strncpy(volume_label, label, 11);

    uint8_t boot_sector[512];
    if (read_sector(partition_start, boot_sector) != 0) {
        return false;
    }

    for (size_t i = 0; i < 11; i++) {
        if (i < strlen(label)) {
            boot_sector[0x47 + i] = label[i];
        } else {
            boot_sector[0x47 + i] = ' ';
        }
    }

    return write_sector(partition_start, boot_sector) == 0;
}

/* ========= 格式化 / 校验 ========= */

bool fat32_format(uint32_t partition_start_sector,
                  const char* volume_label_param) {
    clear_error();

    serial_puts("WARNING: Formatting will erase all data on this partition!\n");

    if (volume_label_param == NULL) {
        volume_label_param = "NO NAME";
    }

    // 1. 获取整个磁盘总扇区数（简化：只支持一个数据盘）
    uint16_t id_buf[256];
    memset(id_buf, 0, sizeof(id_buf));

    // 直接用 IDE IDENTIFY
    ide_wait_ready();
    outb(IDE_DRIVE_HEAD, defult_device);
    outb(IDE_SECTOR_CNT, 0);
    outb(IDE_LBA_LOW, 0);
    outb(IDE_LBA_MID, 0);
    outb(IDE_LBA_HIGH, 0);
    outb(IDE_COMMAND, IDE_CMD_IDENT);

    if (ide_wait_drq() != 0) {
        set_error("IDE IDENTIFY failed");
        return false;
    }

    for (int i = 0; i < 256; i++) {
        id_buf[i] = inw(IDE_DATA);
    }

    // total number of 28-bit LBA sectors is in words 60-61
    uint32_t disk_total_sectors = *(uint32_t*)&id_buf[60];
    if (disk_total_sectors == 0) {
        set_error("Disk reports zero total sectors");
        return false;
    }

    // 2. 计算分区总扇区数（假设只有一个分区：从 partition_start 到磁盘末尾）
    if (partition_start_sector >= disk_total_sectors) {
        set_error("Partition start beyond disk size");
        return false;
    }

    uint32_t partition_total_sectors =
        disk_total_sectors - partition_start_sector;

    // 3. 做低级格式化：写 DBR / FSInfo / FAT / 根目录
    if (!fat32_low_level_format(partition_start_sector,
                                partition_total_sectors,
                                volume_label_param)) {
        return false;
    }

    // 4. 格式化完成后，直接挂载
    return fat32_init(partition_start_sector);
}


bool fat32_check(void) {
    return fat32_format_check();
}

uint32_t fat32_read_sector(uint32_t sector, void* buffer) {
    return read_sector(sector, buffer);
}

uint32_t fat32_write_sector(uint32_t sector,
                            const void* buffer) {
    return write_sector(sector, buffer);
}

/* ========= FSInfo 扇区读写 ========= */

static uint32_t get_cluster_count(uint32_t first_cluster) {
    uint32_t count   = 0;
    uint32_t cluster = first_cluster;

    while (cluster < FAT32_LAST_CLUSTER &&
           cluster != FAT32_FREE_CLUSTER &&
           cluster >= 2) {

        count++;
        cluster = read_fat_entry(cluster);
    }

    return count;
}

static void update_fs_info_sector(void) {
    if (bpb.fs_info_sector == 0 ||
        bpb.fs_info_sector >= bpb.reserved_sectors) {
        return;
    }

    uint8_t fsinfo_sector[512];
    memset(fsinfo_sector, 0, sizeof(fsinfo_sector));

    fsinfo_sector[0x00] = 0x52;
    fsinfo_sector[0x01] = 0x52;
    fsinfo_sector[0x02] = 0x61;
    fsinfo_sector[0x03] = 0x41;

    fsinfo_sector[0x1E4] = 0x72;
    fsinfo_sector[0x1E5] = 0x72;
    fsinfo_sector[0x1E6] = 0x41;
    fsinfo_sector[0x1E7] = 0x61;

    *((uint32_t*)(fsinfo_sector + 0x1E8)) =
        fs_info.free_clusters;

    uint32_t next_free = find_free_cluster();
    *((uint32_t*)(fsinfo_sector + 0x1EC)) =
        next_free;

    fsinfo_sector[0x1FC] = 0x55;
    fsinfo_sector[0x1FD] = 0xAA;

    uint32_t lba = partition_start + bpb.fs_info_sector;
    write_sector(lba, fsinfo_sector);
}

static bool read_fs_info_sector(void) {
    if (bpb.fs_info_sector == 0 ||
        bpb.fs_info_sector >= bpb.reserved_sectors) {
        return false;
    }

    uint8_t fsinfo_sector[512];
    uint32_t lba = partition_start + bpb.fs_info_sector;
    if (read_sector(lba, fsinfo_sector) != 0) {
        return false;
    }

    if (fsinfo_sector[0x00] != 0x52 ||
        fsinfo_sector[0x01] != 0x52 ||
        fsinfo_sector[0x02] != 0x61 ||
        fsinfo_sector[0x03] != 0x41) {
        return false;
    }

    fs_info.free_clusters =
        *((uint32_t*)(fsinfo_sector + 0x1E8));

    return true;
}

static uint32_t find_next_cluster(uint32_t current_cluster,
                                  uint32_t position,
                                  uint32_t bytes_per_cluster) {
    uint32_t target_cluster = position / bytes_per_cluster;
    uint32_t cluster        = current_cluster;

    for (uint32_t i = 0; i < target_cluster; i++) {
        uint32_t next = read_fat_entry(cluster);
        if (next >= FAT32_LAST_CLUSTER ||
            next == FAT32_FREE_CLUSTER ||
            next < 2) {
            return 0;
        }
        cluster = next;
    }

    return cluster;
}

/* ========= toupper ========= */

int toupper(int c) {
    if (c >= 'a' && c <= 'z') {
        return c - ('a' - 'A');
    }
    return c;
}

/* ========= 调试：根目录 dump ========= */

void fat32_debug_dump_root(void) {
    uint32_t sector = cluster_to_sector(fs_info.root_dir_cluster);
    uint8_t buf[512];
    if (read_sector(sector, buf) != 0) {
        serial_puts("debug: read root sector failed\n");
        return;
    }

    serial_puts("=== DEBUG: Root directory entries ===\n");
    for (int offset = 0; offset < 512; offset += 32) {
        fat32_dir_entry_t* e = (fat32_dir_entry_t*)(buf + offset);
        if (e->name[0] == 0x00) {
            serial_puts("  [END]\n");
            break;
        }

        char name[13];
        format_83_name(e->name, name);

        serial_puts("  entry @");
        serial_putdec64(offset);
        serial_puts(": name='");
        serial_puts(name);
        serial_puts("' attr=");
        serial_putdec64(e->attributes);
        serial_puts("\n");
    }
    serial_puts("=== END DEBUG ===\n");
}

static bool fat32_low_level_format(uint32_t partition_start_sector,
                                   uint32_t partition_total_sectors,
                                   const char* volume_label_param) {
    // 1. 选 bytes_per_sector / sectors_per_cluster / reserved_sectors / fat_count
    uint16_t bytes_per_sector    = 512;
    uint8_t  sectors_per_cluster = 1;   // 先用 1，后面根据大小可调
    uint16_t reserved_sectors    = 32;  // 常见保留区大小（含 DBR/FSInfo 等）
    uint8_t  fat_count           = 2;

    uint32_t size_mb = (partition_total_sectors * 512) / (1024 * 1024);
    if (size_mb <= 32) {
        sectors_per_cluster = 1;   // 512
    } else if (size_mb <= 128) {
        sectors_per_cluster = 2;   // 1K
    } else if (size_mb <= 512) {
        sectors_per_cluster = 4;   // 2K
    } else if (size_mb <= 1024) {
        sectors_per_cluster = 8;   // 4K
    } else {
        sectors_per_cluster = 16;  // 8K
    }

    uint32_t total_sectors = partition_total_sectors;

    // 2. 需要先估算一个 FAT 大小（sectors_per_fat）
    // 公式：可用簇数 ≈ 数据区扇区数 / sectors_per_cluster
    // 而 FAT 所占扇区数 ≈ ceil( (簇数 * 4) / bytes_per_sector )
    uint32_t sectors_per_fat = 0;
    {
        uint32_t approx_clusters = 0;
        uint32_t fat_sz = 1;

        for (int iter = 0; iter < 8; iter++) {
            uint32_t data_sec =
                total_sectors - (reserved_sectors + fat_count * fat_sz);
            approx_clusters = data_sec / sectors_per_cluster;
            if (approx_clusters == 0) {
                set_error("Partition too small for FAT32");
                return false;
            }
            uint32_t needed_fat_bytes = (approx_clusters + 2) * 4;
            uint32_t new_fat_sz =
                (needed_fat_bytes + bytes_per_sector - 1) / bytes_per_sector;

            if (new_fat_sz == fat_sz) {
                break;
            }
            fat_sz = new_fat_sz;
        }
        sectors_per_fat = fat_sz;
    }

    // 3. 计算数据区起始扇区 / 总簇数
    uint32_t fat_start_sector  = partition_start_sector + reserved_sectors;
    uint32_t data_start_sector = fat_start_sector + fat_count * sectors_per_fat;

    uint32_t data_sectors =
        total_sectors - (reserved_sectors + fat_count * sectors_per_fat);
    uint32_t total_clusters = data_sectors / sectors_per_cluster;

    if (total_clusters < 65525) {
        // FAT32 要求簇数 >= 65525，否则属于 FAT16 的范围
        set_error("Not enough clusters for FAT32");
        return false;
    }

    // 4. 构造并写入 DBR（Boot Sector）
    uint8_t boot_sector[512];
    memset(boot_sector, 0, sizeof(boot_sector));

    fat32_bpb_t* b = (fat32_bpb_t*)boot_sector;

    // 跳转指令 + OEM
    b->jmp[0] = 0xEB;
    b->jmp[1] = 0x58;
    b->jmp[2] = 0x90;
    memcpy(b->oem, "MSWIN4.1", 8);

    b->bytes_per_sector    = bytes_per_sector;
    b->sectors_per_cluster = sectors_per_cluster;
    b->reserved_sectors    = reserved_sectors;
    b->fat_count           = fat_count;
    b->root_entries        = 0;
    b->total_sectors_16    = 0;
    b->media_type          = 0xF8;
    b->sectors_per_fat_16  = 0;
    b->sectors_per_track   = 63;      // 随便给个常见值
    b->head_count          = 255;
    b->hidden_sectors      = partition_start_sector;
    b->total_sectors_32    = total_sectors;
    b->sectors_per_fat_32  = sectors_per_fat;
    b->flags               = 0;
    b->fat_version         = 0;
    b->root_cluster        = 2;
    b->fs_info_sector      = 1;
    b->backup_boot_sector  = 6;

    memset(b->reserved_ebpb, 0, sizeof(b->reserved_ebpb));
    b->drive_number   = 0x80;
    b->reserved1      = 0;
    b->boot_signature = 0x29;
    b->volume_id      = 0x12345678;

    memset(b->volume_label, ' ', sizeof(b->volume_label));
    size_t vlen = strlen(volume_label_param);
    if (vlen > 11) vlen = 11;
    memcpy(b->volume_label, volume_label_param, vlen);

    memcpy(b->fs_type, "FAT32   ", 8);

    boot_sector[510] = 0x55;
    boot_sector[511] = 0xAA;

    if (write_sector(partition_start_sector, boot_sector) != 0) {
        set_error("Failed to write boot sector");
        return false;
    }

    // 5. 写备份引导扇区（通常在扇区 6）
    if (write_sector(partition_start_sector + b->backup_boot_sector,
                     boot_sector) != 0) {
        set_error("Failed to write backup boot sector");
        return false;
    }

    // 6. 写 FSInfo 扇区（扇区 1）
    uint8_t fsinfo_sector[512];
    memset(fsinfo_sector, 0, sizeof(fsinfo_sector));
    fsinfo_sector[0x00] = 0x52;
    fsinfo_sector[0x01] = 0x52;
    fsinfo_sector[0x02] = 0x61;
    fsinfo_sector[0x03] = 0x41;

    fsinfo_sector[0x1E4] = 0x72;
    fsinfo_sector[0x1E5] = 0x72;
    fsinfo_sector[0x1E6] = 0x41;
    fsinfo_sector[0x1E7] = 0x61;

    uint32_t free_clusters = total_clusters - 1; // root cluster 占 1 个
    *((uint32_t*)(fsinfo_sector + 0x1E8)) = free_clusters;
    *((uint32_t*)(fsinfo_sector + 0x1EC)) = 3;   // next free cluster hint

    fsinfo_sector[0x1FC] = 0x55;
    fsinfo_sector[0x1FD] = 0xAA;

    if (write_sector(partition_start_sector + 1, fsinfo_sector) != 0) {
        set_error("Failed to write FSInfo");
        return false;
    }

    // 7. 初始化 FAT1 / FAT2
    uint32_t fat1_lba = fat_start_sector;
    uint32_t fat2_lba = fat_start_sector + sectors_per_fat;

    uint8_t fat_sector[512];
    memset(fat_sector, 0, sizeof(fat_sector));

    // FAT32 需要在 FAT[0] / FAT[1] 中写入一些保留值 + root cluster
    // cluster 0: media descriptor + reserved bits
    // cluster 1: reserved (0xFFFFFFFF)
    // cluster 2: root directory cluster (结束标记)
    uint32_t* fat_entries = (uint32_t*)fat_sector;
    fat_entries[0] = 0x0FFFFFF8 | 0xF8;   // 媒体描述符在低 8 位，但通常写 0x0FFFFFF8
    fat_entries[1] = 0x0FFFFFFF;         // 保留
    fat_entries[2] = 0x0FFFFFFF;         // 根目录簇（簇 2）结束标记

    // 写 FAT1 第一个扇区
    if (write_sector(fat1_lba, fat_sector) != 0) {
        set_error("Failed to write FAT1[0]");
        return false;
    }
    // 其余 FAT1 扇区清零
    memset(fat_sector, 0, sizeof(fat_sector));
    for (uint32_t i = 1; i < sectors_per_fat; i++) {
        if (write_sector(fat1_lba + i, fat_sector) != 0) {
            set_error("Failed to clear FAT1");
            return false;
        }
    }

    // FAT2 同样处理
    // 第一个扇区写和 FAT1 一样的内容
    uint8_t fat2_first[512];
    memset(fat2_first, 0, sizeof(fat2_first));
    uint32_t* fat2_entries = (uint32_t*)fat2_first;
    fat2_entries[0] = 0x0FFFFFF8 | 0xF8;
    fat2_entries[1] = 0x0FFFFFFF;
    fat2_entries[2] = 0x0FFFFFFF;

    if (write_sector(fat2_lba, fat2_first) != 0) {
        set_error("Failed to write FAT2[0]");
        return false;
    }
    memset(fat_sector, 0, sizeof(fat_sector));
    for (uint32_t i = 1; i < sectors_per_fat; i++) {
        if (write_sector(fat2_lba + i, fat_sector) != 0) {
            set_error("Failed to clear FAT2");
            return false;
        }
    }

    // 8. 清空根目录簇（cluster 2）
    uint32_t root_cluster = 2;
    uint32_t root_sector  = data_start_sector; // cluster 2 对应 data_start
    uint8_t zero_buf[512];
    memset(zero_buf, 0, sizeof(zero_buf));
    for (uint32_t i = 0; i < sectors_per_cluster; i++) {
        if (write_sector(root_sector + i, zero_buf) != 0) {
            set_error("Failed to clear root directory");
            return false;
        }
    }

    serial_puts("FAT32 low-level format completed.\n");
    return true;
}

uint32_t detect_fat32_partition(void) {
    uint8_t sector[512];

    // 读取 LBA 0
    if (ide_read_sectors(0, 1, sector) != 0) {
        serial_puts("Failed to read LBA 0\n");
        return 0;
    }

    // 检查签名 0x55AA
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        serial_puts("Invalid boot signature\n");
        return 0;
    }

    serial_puts("Boot signature OK\n");

    // 检查是否是 FAT32 BPB（偏移 0x52 处是 "FAT32   "）
    if (sector[0x52] == 'F' && sector[0x53] == 'A' && 
        sector[0x54] == 'T' && sector[0x55] == '3' &&
        sector[0x56] == '2') {
        serial_puts("FAT32 superfloppy detected (no partition table)\n");
        return 0;  // 整个磁盘就是 FAT32，从 LBA 0 开始
    }

    // 否则尝试解析 MBR 分区表
    mbr_partition_entry_t* parts =
        (mbr_partition_entry_t*)(sector + 446);

    uint32_t best_lba = 0;

    for (int i = 0; i < 4; i++) {
        mbr_partition_entry_t* p = &parts[i];

        if (p->partition_type == 0) {
            continue;
        }

        serial_puts("MBR partition ");
        serial_putdec64(i);
        serial_puts(": type=0x");
        serial_puthex8(p->partition_type);
        serial_puts(" start_lba=");
        serial_putdec64(p->start_lba);
        serial_puts(" total_sectors=");
        serial_putdec64(p->total_sectors);
        serial_puts("\n");

        // FAT32 常见类型：0x0B (CHS), 0x0C (LBA)
        if (p->partition_type == 0x0B ||
            p->partition_type == 0x0C) {

            if (best_lba == 0) {
                best_lba = p->start_lba;
            }
        }
    }

    if (best_lba == 0) {
        serial_puts("No FAT32 partition found, assuming superfloppy\n");
    } else {
        serial_puts("FAT32 partition found at LBA: ");
        serial_putdec64(best_lba);
        serial_puts("\n");
    }

    return best_lba;
}

bool fat32_read_all_fast(fat32_handle_t* handle, void* buffer)
{
    clear_error();

    if (!fs_mounted || !handle || !buffer || !handle->is_open) {
        set_error("Invalid parameters");
        return false;
    }

    if (handle->is_directory) {
        set_error("Cannot fast-read a directory");
        return false;
    }

    if (handle->file_size == 0) {
        return true;
    }

    if (handle->position != 0) {
        set_error("fast_read requires position == 0");
        return false;
    }

    uint32_t bytes_per_cluster =
        fs_info.sectors_per_cluster * fs_info.bytes_per_sector;

    uint32_t total_clusters =
        (handle->file_size + bytes_per_cluster - 1) / bytes_per_cluster;

    // 为簇链分配一个临时数组（10MB / 2KB ≈ 5000 簇，很安全）
    uint32_t* chain = (uint32_t*)kmalloc(total_clusters * sizeof(uint32_t));
    if (!chain) {
        set_error("No memory for cluster chain");
        return false;
    }

    // 构建簇链：cluster[0] = first_cluster，后面顺着 FAT 走
    uint32_t c = handle->first_cluster;
    for (uint32_t i = 0; i < total_clusters; i++) {
        if (c < 2 || c >= FAT32_LAST_CLUSTER) {
            kfree(chain);
            set_error("Invalid cluster chain");
            return false;
        }
        chain[i] = c;
        c = read_fat_entry(c);
    }

    uint8_t* dst = (uint8_t*)buffer;
    uint32_t bytes_left = handle->file_size;

    uint32_t i = 0;
    while (i < total_clusters && bytes_left > 0) {
        // 找一段连续簇：cluster[i], cluster[i+1] = +1, ...
        uint32_t run_start = i;
        uint32_t run_len   = 1;

        while (run_start + run_len < total_clusters) {
            if (chain[run_start + run_len] != chain[run_start + run_len - 1] + 1)
                break;
            run_len++;
        }

        uint32_t run_clusters = run_len;
        uint32_t run_bytes    = run_clusters * bytes_per_cluster;

        if (run_bytes > bytes_left) {
            run_bytes = bytes_left;
            // 最后一段不一定整簇，但 ide_read_auto 按字节读没问题
        }

        uint32_t lba = cluster_to_sector(chain[run_start]);

        if (!ide_read_auto(lba, run_bytes, dst)) {
            kfree(chain);
            set_error("multi-cluster read failed");
            return false;
        }

        dst        += run_bytes;
        bytes_left -= run_bytes;
        i          += run_clusters;
    }

    kfree(chain);

    handle->position        = handle->file_size;
    handle->current_cluster = chain[total_clusters - 1];

    return (bytes_left == 0);
}
