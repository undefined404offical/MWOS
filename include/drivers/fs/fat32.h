#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FS_FAT32 0x0C

#pragma once
#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint8_t  boot_indicator;  // 0x80 = bootable, 0x00 = non-bootable
    uint8_t  start_head;
    uint8_t  start_sector;    // bits 0-5 sector, bits 6-7 high bits of cylinder
    uint8_t  start_cylinder;
    uint8_t  partition_type;  // 0x0B,0x0C = FAT32, etc.
    uint8_t  end_head;
    uint8_t  end_sector;
    uint8_t  end_cylinder;
    uint32_t start_lba;       // LBA of first sector in partition
    uint32_t total_sectors;   // number of sectors in partition
} mbr_partition_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t  bootstrap[446];
    mbr_partition_entry_t partitions[4];
    uint16_t signature;       // 0xAA55
} mbr_t;


// FAT32 BIOS Parameter Block (BPB)
typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];              // 0
    char     oem[8];              // 3
    uint16_t bytes_per_sector;    // 11 (0x0B)
    uint8_t  sectors_per_cluster; // 13 (0x0D)
    uint16_t reserved_sectors;    // 14 (0x0E)
    uint8_t  fat_count;           // 16 (0x10)
    uint16_t root_entries;        // 17 (0x11) = 0 for FAT32
    uint16_t total_sectors_16;    // 19 (0x13)
    uint8_t  media_type;          // 21 (0x15)
    uint16_t sectors_per_fat_16;  // 22 (0x16)
    uint16_t sectors_per_track;   // 24 (0x18)
    uint16_t head_count;          // 26 (0x1A)
    uint32_t hidden_sectors;      // 28 (0x1C)
    uint32_t total_sectors_32;    // 32 (0x20)

    // FAT32 Extended BPB
    uint32_t sectors_per_fat_32;  // 36 (0x24)
    uint16_t flags;               // 40 (0x28)
    uint16_t fat_version;         // 42 (0x2A)
    uint32_t root_cluster;        // 44 (0x2C)
    uint16_t fs_info_sector;      // 48 (0x30)
    uint16_t backup_boot_sector;  // 50 (0x32)
    uint8_t  reserved_ebpb[12];   // 52 (0x34) - 63 (0x3F)

    uint8_t  drive_number;        // 64 (0x40)
    uint8_t  reserved1;           // 65 (0x41)
    uint8_t  boot_signature;      // 66 (0x42)
    uint32_t volume_id;           // 67 (0x43)
    char     volume_label[11];    // 71 (0x47)
    char     fs_type[8];          // 82 (0x52)
} fat32_bpb_t;



_Static_assert(sizeof(fat32_bpb_t) == 90, "FAT32 BPB size must be 90 bytes");


typedef struct __attribute__((packed)) {
    char        name[11];
    uint8_t     attributes;
    uint8_t     reserved;
    uint8_t     creation_time_tenths;
    uint16_t    creation_time;
    uint16_t    creation_date;
    uint16_t    last_access_date;
    uint16_t    cluster_high;
    uint16_t    last_write_time;
    uint16_t    last_write_date;
    uint16_t    cluster_low;
    uint32_t    file_size;
} fat32_dir_entry_t;

// FAT32 File/Directory Handle
typedef struct {
    uint32_t    first_cluster;         // First cluster of file/directory
    uint32_t    current_cluster;
    uint32_t    current_offset;
    uint32_t    file_size;
    uint32_t    position;              // Current read/write position
    bool        is_directory;          // True if this is a directory
    bool        is_open;               // True if handle is open
    uint8_t     buffer[512];
    uint32_t    buffer_sector;
    bool        buffer_dirty;

    uint32_t    dir_sector;
    uint32_t    dir_offset;
} fat32_handle_t;

typedef struct {
    uint32_t    sectors_per_cluster;
    uint32_t    bytes_per_sector;
    uint32_t    total_sectors;
    uint32_t    fat_start_sector;
    uint32_t    fat_sectors;
    uint32_t    data_start_sector;
    uint32_t    root_dir_cluster;
    uint32_t    total_clusters;
    uint32_t    free_clusters;
    uint8_t     fat_number;            // Which FAT to use (usually 0)
} fat32_info_t;

typedef struct {
    uint32_t dir_cluster;          // 包含该项的目录簇
    fat32_dir_entry_t entry;       // 找到的目录项
    uint32_t entry_sector;         // 该目录项所在扇区
    uint32_t entry_offset;         // 扇区内偏移（字节）
    bool found;
} fat32_path_result_t;

#define ATTR_READ_ONLY  0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20
#define ATTR_LONG_NAME  0x0F

#define FAT32_FREE_CLUSTER     0x00000000
#define FAT32_RESERVED_CLUSTER 0x00000001
#define FAT32_BAD_CLUSTER      0x0FFFFFF7
#define FAT32_LAST_CLUSTER     0x0FFFFFF8
#define FAT32_CLUSTER_MASK     0x0FFFFFFF

typedef enum {
    FILE_READ,
    FILE_WRITE,
    FILE_APPEND,
    FILE_CREATE
} file_mode_t;

bool fat32_init(uint32_t partition_start);
bool fat32_mount(uint32_t partition_start);
void fat32_umount(void);
bool fat32_format(uint32_t partition_start, const char* volume_label);
bool fat32_check(void);
uint32_t fat32_get_free_space(void);
uint32_t fat32_get_total_space(void);
const char* fat32_get_volume_label(void);
bool fat32_set_volume_label(const char* label);

bool fat32_open(const char* path, fat32_handle_t* handle, file_mode_t mode);
bool fat32_open_root(fat32_handle_t* handle);
bool fat32_read(fat32_handle_t* handle, void* buffer, uint32_t size);
bool fat32_write(fat32_handle_t* handle, const void* buffer, uint32_t size);
bool fat32_seek(fat32_handle_t* handle, uint32_t position);
bool fat32_truncate(fat32_handle_t* handle, uint32_t new_size);
void fat32_close(fat32_handle_t* handle);

bool fat32_create_dir(const char* path);
bool fat32_remove_dir(const char* path);
bool fat32_read_dir(fat32_handle_t* dir_handle, fat32_dir_entry_t* entry);
bool fat32_find_file(const char* path, fat32_dir_entry_t* entry);

bool fat32_create_file(const char* path);
bool fat32_delete_file(const char* path);
bool fat32_rename(const char* old_path, const char* new_path);
bool fat32_copy(const char* src_path, const char* dst_path);
bool fat32_move(const char* src_path, const char* dst_path);
bool fat32_file_exists(const char* path);
uint32_t fat32_get_file_size(const char* path);
bool fat32_get_file_info(const char* path, fat32_dir_entry_t* info);
bool fat32_set_file_attributes(const char* path, uint8_t attributes);

const char* fat32_get_error(void);
bool fat32_mounted(void);
bool fat32_format_check(void);
void fat32_print_info(void);

// Disk I/O functions
uint32_t fat32_read_sector(uint32_t sector, void* buffer);
uint32_t fat32_write_sector(uint32_t sector, const void* buffer);

int toupper(int c);

void fat32_debug_dump_root(void);
bool fat32_format(uint32_t partition_start_sector,
                  const char* volume_label_param);
uint32_t detect_fat32_partition(void);
void format_83_name(const char src[11], char dest[13]);
bool fat32_read_all_fast(fat32_handle_t* handle, void* buffer);

#endif
