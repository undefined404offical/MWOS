#include "drivers/fs/ext2.h"
#include "drivers/fs/fscache.h"
#include "drivers/fs/vfs.h"
#include "drivers/disk.h"
#include "drivers/ide.h"
#include "serial.h"
#include "string.h"
#include "memory.h"
#include "io.h"

static ext2_fs_info_t g_fs;
static bool g_mounted = false;
static char g_error[64] = {0};

#define SECTOR_SIZE 512

static void set_error(const char* msg)
{
    strncpy(g_error, msg, sizeof(g_error) - 1);
    g_error[sizeof(g_error) - 1] = '\0';
}

// ============================================
// 底层块 I/O
// ============================================

// EXT2 块编号 → LBA（注意：块 0 和块 1 是引导区/超级块预留的）
static inline uint32_t block_to_lba(uint32_t block)
{
    // 块大小至少 1024，扇区大小 512
    // block 0 对应 LBA = partition_start + 0
    // block 1 对应 LBA = partition_start + block_size / 512
    return g_fs.partition_start + block * (g_fs.block_size / SECTOR_SIZE);
}

// 读取一个 EXT2 块到 buffer（buffer 至少 block_size 字节）
static bool read_block(uint32_t block, void* buffer)
{
    uint32_t lba = block_to_lba(block);
    uint32_t sectors = g_fs.block_size / SECTOR_SIZE;  // 8 sectors for 4K blocks

    // 直接通过 IDE 批量读取（比逐扇区 fscache 更快）
    if (ide_read_sectors(lba, sectors, buffer) != 0) {
        set_error("Block read failed");
        return false;
    }
    return true;
}

// 写入一个 EXT2 块
static bool write_block(uint32_t block, const void* buffer)
{
    uint32_t lba = block_to_lba(block);
    uint32_t sectors = g_fs.block_size / SECTOR_SIZE;

    for (uint32_t i = 0; i < sectors; i++) {
        if (!fscache_write_sector(0, lba + i, (const uint8_t*)buffer + i * SECTOR_SIZE)) {
            set_error("Block write failed");
            return false;
        }
    }
    return true;
}

// ============================================
// 块组描述符访问
// ============================================

// 读取块组描述符表
// 块组描述符表位于第一个数据块之后的连续块中
// 如果 block_size = 1024，则 BGDT 从 block 2 开始
// 如果 block_size > 1024，则 BGDT 从 block 1 开始
static bool read_bgdt(void)
{
    // BGDT 起始块
    uint32_t bgdt_block = (g_fs.block_size == 1024) ? 2 : 1;
    uint32_t bgdt_size = g_fs.bg_desc_count * sizeof(ext2_bg_desc_t);
    uint32_t bgdt_blocks = (bgdt_size + g_fs.block_size - 1) / g_fs.block_size;

    g_fs.bg_desc_blocks = bgdt_blocks;

    // 分配缓冲区（用于后续逐个读取块组描述符）
    // 实际上我们要自己管理，先给静态数组留够空间
    serial_puts("EXT2: BGDT at block ");
    serial_putdec32(bgdt_block);
    serial_puts(", ");
    serial_putdec32(bgdt_blocks);
    serial_puts(" blocks, ");
    serial_putdec32(g_fs.bg_desc_count);
    serial_puts(" groups\n");

    return true;
}

// 获取第 i 个块组描述符
static bool get_bg_desc(uint32_t i, ext2_bg_desc_t* bg)
{
    if (i >= g_fs.bg_desc_count) {
        set_error("Block group out of range");
        return false;
    }

    uint32_t bgdt_block = (g_fs.block_size == 1024) ? 2 : 1;
    uint32_t entry_size = sizeof(ext2_bg_desc_t);
    uint32_t entries_per_block = g_fs.block_size / entry_size;

    uint32_t block = bgdt_block + i / entries_per_block;
    uint32_t offset = (i % entries_per_block) * entry_size;

    uint8_t buf[4096]; // 足够大（最大块大小 4096）
    if (!read_block(block, buf)) {
        set_error("Failed to read BGDT block");
        return false;
    }

    memcpy(bg, buf + offset, entry_size);
    return true;
}

// ============================================
// Inode 操作
// ============================================

// 读取 inode
static bool ext2_read_inode(uint32_t inode_no, ext2_inode_t* inode)
{
    if (inode_no == 0 || inode_no > g_fs.sb.inodes_count) {
        set_error("Invalid inode number");
        return false;
    }

    uint32_t group = (inode_no - 1) / g_fs.inodes_per_group;
    uint32_t index = (inode_no - 1) % g_fs.inodes_per_group;

    ext2_bg_desc_t bg;
    if (!get_bg_desc(group, &bg)) return false;

    uint32_t inode_table_block = bg.inode_table;
    uint32_t inode_size = g_fs.sb.inode_size;
    if (inode_size == 0) inode_size = 128;

    uint32_t inodes_per_block = g_fs.block_size / inode_size;
    uint32_t block = inode_table_block + index / inodes_per_block;
    uint32_t offset = (index % inodes_per_block) * inode_size;

    uint8_t buf[4096];
    if (!read_block(block, buf)) {
        set_error("Failed to read inode table block");
        return false;
    }

    memcpy(inode, buf + offset, sizeof(ext2_inode_t));
    return true;
}

// 写入 inode
static bool ext2_write_inode(uint32_t inode_no, const ext2_inode_t* inode)
{
    uint32_t group = (inode_no - 1) / g_fs.inodes_per_group;
    uint32_t index = (inode_no - 1) % g_fs.inodes_per_group;

    ext2_bg_desc_t bg;
    if (!get_bg_desc(group, &bg)) return false;

    uint32_t inode_table_block = bg.inode_table;
    uint32_t inode_size = g_fs.sb.inode_size;
    if (inode_size == 0) inode_size = 128;

    uint32_t inodes_per_block = g_fs.block_size / inode_size;
    uint32_t block = inode_table_block + index / inodes_per_block;
    uint32_t offset = (index % inodes_per_block) * inode_size;

    uint8_t buf[4096];
    if (!read_block(block, buf)) {
        set_error("Failed to read inode table block for write");
        return false;
    }

    memcpy(buf + offset, inode, sizeof(ext2_inode_t));
    return write_block(block, buf);
}

// ============================================
// 块指针解析（处理间接块）
// ============================================

// 读取间接块中的指针表，返回第 index 个指针
static bool read_indirect_block_ptr(uint32_t block, uint32_t index, uint32_t* result)
{
    uint32_t ptrs_per_block = g_fs.block_size / 4;
    if (index >= ptrs_per_block) return false;

    uint8_t buf[4096];
    if (!read_block(block, buf)) return false;

    *result = ((uint32_t*)buf)[index];
    return true;
}

// 解析 inode 中第 block_idx 个逻辑块的物理块号
static bool ext2_get_block_ptr(const ext2_inode_t* inode, uint32_t block_idx, uint32_t* phys_block)
{
    uint32_t ptrs_per_block = g_fs.block_size / 4;
    uint32_t ptrs_per_dind = ptrs_per_block * ptrs_per_block;

    if (block_idx < 12) {
        // 直接块指针
        *phys_block = inode->block[block_idx];
        return true;
    }

    block_idx -= 12;

    if (block_idx < ptrs_per_block) {
        // 一级间接
        if (inode->block[12] == 0) return false;
        return read_indirect_block_ptr(inode->block[12], block_idx, phys_block);
    }

    block_idx -= ptrs_per_block;

    if (block_idx < ptrs_per_dind) {
        // 二级间接
        if (inode->block[13] == 0) return false;

        uint32_t indir_idx = block_idx / ptrs_per_block;
        uint32_t direct_idx = block_idx % ptrs_per_block;

        uint32_t indir_block;
        if (!read_indirect_block_ptr(inode->block[13], indir_idx, &indir_block))
            return false;

        return read_indirect_block_ptr(indir_block, direct_idx, phys_block);
    }

    block_idx -= ptrs_per_dind;
    // 三级间接（通常用不到，简单实现）
    uint32_t ptrs_per_tind = ptrs_per_block * ptrs_per_dind;

    if (block_idx < ptrs_per_tind) {
        if (inode->block[14] == 0) return false;

        uint32_t dind_idx = block_idx / ptrs_per_dind;
        uint32_t rem = block_idx % ptrs_per_dind;
        uint32_t indir_idx = rem / ptrs_per_block;
        uint32_t direct_idx = rem % ptrs_per_block;

        uint32_t dind_block;
        if (!read_indirect_block_ptr(inode->block[14], dind_idx, &dind_block))
            return false;

        uint32_t indir_block;
        if (!read_indirect_block_ptr(dind_block, indir_idx, &indir_block))
            return false;

        return read_indirect_block_ptr(indir_block, direct_idx, phys_block);
    }

    return false;
}

// 读取 inode 中第 block_idx 个逻辑块的数据到 buffer
static bool ext2_read_inode_block(const ext2_inode_t* inode, uint32_t block_idx, void* buffer)
{
    uint32_t phys_block;
    if (!ext2_get_block_ptr(inode, block_idx, &phys_block)) {
        // 块未分配，用零填充
        memset(buffer, 0, g_fs.block_size);
        return true;
    }
    if (phys_block == 0) {
        memset(buffer, 0, g_fs.block_size);
        return true;
    }
    return read_block(phys_block, buffer);
}

// ============================================
// 路径解析与目录项查找
// ============================================

// 将路径分割为组件并递归查找
// 返回目标 inode 及其父目录 inode
static bool ext2_find_entry(const char* path, ext2_inode_t* dir_inode,
                            uint32_t* dir_ino, ext2_dir_entry_t* found_entry,
                            uint32_t* parent_ino)
{
    if (!path || !*path) return false;

    // 从根 inode 开始
    uint32_t current_ino = EXT2_ROOT_INO;
    ext2_inode_t current_inode;

    if (!ext2_read_inode(current_ino, &current_inode)) {
        set_error("Failed to read root inode");
        return false;
    }

    // 跳过开头的 '/'
    const char* p = path;
    while (*p == '/') p++;

    if (*p == '\0') {
        // 根目录本身
        if (dir_inode) memcpy(dir_inode, &current_inode, sizeof(ext2_inode_t));
        if (dir_ino) *dir_ino = current_ino;
        if (parent_ino) *parent_ino = current_ino;
        return true;
    }

    uint32_t parent_ino_val = current_ino;

    // 逐层解析
    while (*p) {
        // 提取路径分量
        char component[256];
        int comp_len = 0;
        while (*p && *p != '/' && comp_len < 255) {
            component[comp_len++] = *p;
            p++;
        }
        component[comp_len] = '\0';

        parent_ino_val = current_ino;

        // 在当前目录 inode 中查找组件
        bool found = false;
        uint32_t block_idx = 0;
        uint8_t block_buf[4096];

        while (1) {
            memset(block_buf, 0, sizeof(block_buf));
            if (!ext2_read_inode_block(&current_inode, block_idx, block_buf)) break;

            // 检查这个块中是否有数据
            bool has_data = false;
            uint32_t offset = 0;
            while (offset < g_fs.block_size) {
                ext2_dir_entry_t* entry = (ext2_dir_entry_t*)(block_buf + offset);

                if (entry->inode == 0) {
                    // 跳过无效/已删除项
                    if (entry->rec_len == 0) break;
                    offset += entry->rec_len;
                    continue;
                }

                if (entry->rec_len == 0) break;
                has_data = true;

                // 检查名称是否匹配
                if (entry->name_len == comp_len &&
                    memcmp(entry->name, component, comp_len) == 0) {

                    current_ino = entry->inode;

                    if (!ext2_read_inode(current_ino, &current_inode)) {
                        set_error("Failed to read inode for path component");
                        return false;
                    }

                    if (found_entry) {
                        found_entry->inode = entry->inode;
                        found_entry->rec_len = entry->rec_len;
                        found_entry->name_len = entry->name_len;
                        found_entry->file_type = entry->file_type;
                    }

                    found = true;
                    break;
                }

                offset += entry->rec_len;
            }

            if (found) break;
            if (!has_data) break;

            block_idx++;
        }

        if (!found) {
            set_error("Path component not found");
            return false;
        }

        // 跳过连续的 '/'
        while (*p == '/') p++;
    }

    if (dir_inode) memcpy(dir_inode, &current_inode, sizeof(ext2_inode_t));
    if (dir_ino) *dir_ino = current_ino;
    if (parent_ino) *parent_ino = parent_ino_val;

    return true;
}

// ============================================
// 磁盘块分配
// ============================================

// 在位图中查找空闲块
static uint32_t alloc_block(void)
{
    // 遍历所有块组
    for (uint32_t g = 0; g < g_fs.bg_desc_count; g++) {
        ext2_bg_desc_t bg;
        if (!get_bg_desc(g, &bg)) continue;

        if (bg.free_blocks_count == 0) continue;

        // 读取块位图
        uint8_t bitmap[4096];
        if (!read_block(bg.block_bitmap, bitmap)) continue;

        // 找出第一个空闲块
        uint32_t blocks_in_group = g_fs.blocks_per_group;
        if (g == g_fs.bg_desc_count - 1) {
            // 最后一组可能不满
            blocks_in_group = g_fs.sb.blocks_count - g * g_fs.blocks_per_group;
        }

        for (uint32_t i = 0; i < blocks_in_group; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                // 标记为已使用
                bitmap[i / 8] |= (1 << (i % 8));
                if (!write_block(bg.block_bitmap, bitmap)) return 0;

                // 更新 BGDT
                bg.free_blocks_count--;
                // 简化处理：不写回 BGDT（系统重启后 fsck 修复）
                // 实际需要：计算 BGDT 所在块并写回

                uint32_t block = g * g_fs.blocks_per_group + i;
                return block;
            }
        }
    }

    set_error("No free blocks available");
    return 0;
}

// 在 inode 位图中查找空闲 inode
static uint32_t alloc_inode(void)
{
    for (uint32_t g = 0; g < g_fs.bg_desc_count; g++) {
        ext2_bg_desc_t bg;
        if (!get_bg_desc(g, &bg)) continue;

        if (bg.free_inodes_count == 0) continue;

        uint8_t bitmap[4096];
        if (!read_block(bg.inode_bitmap, bitmap)) continue;

        uint32_t inodes_in_group = g_fs.inodes_per_group;
        if (g == g_fs.bg_desc_count - 1) {
            inodes_in_group = g_fs.sb.inodes_count - g * g_fs.inodes_per_group;
        }

        for (uint32_t i = 0; i < inodes_in_group; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                bitmap[i / 8] |= (1 << (i % 8));
                if (!write_block(bg.inode_bitmap, bitmap)) return 0;

                uint32_t inode_no = g * g_fs.inodes_per_group + i + 1;
                return inode_no;
            }
        }
    }

    set_error("No free inodes available");
    return 0;
}

// 将一个物理块添加到 inode 的块指针中
static bool ext2_add_block_to_inode(ext2_inode_t* inode, uint32_t phys_block)
{
    // 找第一个空闲块指针位置（使用 0 表示空闲）
    for (int i = 0; i < 12; i++) {
        if (inode->block[i] == 0) {
            inode->block[i] = phys_block;
            return true;
        }
    }

    // 处理间接块 - 简化：使用一级间接
    uint32_t ptrs_per_block = g_fs.block_size / 4;
    if (inode->block[12] == 0) {
        // 需要分配一个间接块
        uint32_t indir_block = alloc_block();
        if (indir_block == 0) return false;

        uint8_t zero[4096];
        memset(zero, 0, g_fs.block_size);
        write_block(indir_block, zero);

        inode->block[12] = indir_block;
    }

    // 读取间接块，找空闲位置
    uint8_t buf[4096];
    if (!read_block(inode->block[12], buf)) return false;

    uint32_t* ptrs = (uint32_t*)buf;
    for (uint32_t i = 0; i < ptrs_per_block; i++) {
        if (ptrs[i] == 0) {
            ptrs[i] = phys_block;
            return write_block(inode->block[12], buf);
        }
    }

    set_error("Inode block pointer table full");
    return false;
}

// 初始化 inode 位图（格式化用）
static void mark_inode_bitmap(uint32_t inode_no)
{
    uint32_t group = (inode_no - 1) / g_fs.inodes_per_group;
    uint32_t index = (inode_no - 1) % g_fs.inodes_per_group;

    uint8_t buf[4096]; // 临时用，实际应读现有位图
    memset(buf, 0, sizeof(buf));
}

// ============================================
// 目录操作
// ============================================

// 在目录中添加一个目录项
static bool ext2_add_dir_entry(uint32_t dir_ino, ext2_inode_t* dir_inode,
                               const char* name, uint32_t inode_no, uint8_t file_type)
{
    int name_len = strlen(name);
    if (name_len > 255) name_len = 255;

    // 计算需要的记录长度（对齐到 4 字节）
    uint32_t needed = sizeof(ext2_dir_entry_t) + name_len;
    needed = (needed + 3) & ~3;

    // 遍历目录块查找空闲位置
    uint8_t block_buf[4096];
    uint32_t block_idx = 0;

    while (1) {
        memset(block_buf, 0, sizeof(block_buf));
        bool has_data = false;

        if (!ext2_read_inode_block(dir_inode, block_idx, block_buf)) break;

        uint32_t offset = 0;
        uint32_t last_entry_offset = 0;
        uint32_t last_entry_rec_len = 0;

        while (offset < g_fs.block_size) {
            ext2_dir_entry_t* entry = (ext2_dir_entry_t*)(block_buf + offset);

            if (entry->inode == 0 && entry->rec_len == 0) {
                // 未初始化的块末尾
                break;
            }

            if (entry->inode == 0 || entry->rec_len == 0) {
                break;
            }

            has_data = true;

            // 检查是否可以复用这个条目（rec_len 远大于实际需要）
            uint32_t actual_size = sizeof(ext2_dir_entry_t) + entry->name_len;
            actual_size = (actual_size + 3) & ~3;
            uint32_t slack = entry->rec_len - actual_size;

            if (slack >= needed) {
                // 可以在此条目中分裂出新的条目
                // 修改当前条目，缩小 rec_len
                entry->rec_len = actual_size;
                // 在末尾添加新条目
                ext2_dir_entry_t* new_entry = (ext2_dir_entry_t*)(block_buf + offset + actual_size);
                new_entry->inode = inode_no;
                new_entry->rec_len = slack;
                new_entry->name_len = name_len;
                new_entry->file_type = file_type;
                memcpy(new_entry->name, name, name_len);

                // 写回目录块
                uint32_t phys_block;
                if (ext2_get_block_ptr(dir_inode, block_idx, &phys_block) && phys_block) {
                    return write_block(phys_block, block_buf);
                }
                return false;
            }

            last_entry_offset = offset;
            last_entry_rec_len = entry->rec_len;
            offset += entry->rec_len;
        }

        if (!has_data && block_idx == 0) {
            // 空目录，在块开始处添加条目
            // rec_len 覆盖整个块（如果是最后一个条目）或者留到块末尾
            // 简化：直接填充整个块
            uint32_t remaining = g_fs.block_size;

            ext2_dir_entry_t* entry = (ext2_dir_entry_t*)block_buf;
            entry->inode = inode_no;
            entry->rec_len = remaining;
            entry->name_len = name_len;
            entry->file_type = file_type;
            memcpy(entry->name, name, name_len);

            // 写回
            uint32_t phys_block;
            if (ext2_get_block_ptr(dir_inode, block_idx, &phys_block) && phys_block) {
                return write_block(phys_block, block_buf);
            }
            return false;
        }

        // 当前块末尾有空间？可以扩展最后一个条目直接到块末尾
        if (has_data && last_entry_offset + last_entry_rec_len < g_fs.block_size) {
            uint32_t free_space = g_fs.block_size - (last_entry_offset + last_entry_rec_len);
            if (free_space >= needed) {
                // 扩展最后一个条目的 rec_len 到块末尾
                ext2_dir_entry_t* last_entry = (ext2_dir_entry_t*)(block_buf + last_entry_offset);
                last_entry->rec_len += free_space;

                // 在末尾添加新条目
                ext2_dir_entry_t* new_entry = (ext2_dir_entry_t*)(block_buf + last_entry_offset + last_entry_rec_len);
                new_entry->inode = inode_no;
                new_entry->rec_len = free_space;
                new_entry->name_len = name_len;
                new_entry->file_type = file_type;
                memcpy(new_entry->name, name, name_len);

                uint32_t phys_block;
                if (ext2_get_block_ptr(dir_inode, block_idx, &phys_block) && phys_block) {
                    return write_block(phys_block, block_buf);
                }
                return false;
            }
        }

        block_idx++;
        // 检查是否还有更多块（通过 inode->blocks 字段）
        if (block_idx >= (dir_inode->blocks * (512 / g_fs.block_size))) {
            // 分配新块
            uint32_t new_block = alloc_block();
            if (new_block == 0) return false;

            uint8_t zero[4096];
            memset(zero, 0, g_fs.block_size);
            write_block(new_block, zero);

            if (!ext2_add_block_to_inode(dir_inode, new_block)) return false;

            // 更新 inode 大小
            dir_inode->size += g_fs.block_size;
            dir_inode->blocks += g_fs.block_size / 512;
            ext2_write_inode(dir_ino, dir_inode);

            // 在新块中添加条目
            // 递归调用自身（但这次新块存在）
            break;
        }
    }

    // 在新块中添加条目（兜底）
    // 再次读取最新的 inode
    ext2_inode_t updated_inode;
    ext2_read_inode(dir_ino, &updated_inode);

    uint8_t last_buf[4096];
    if (!ext2_read_inode_block(&updated_inode, block_idx, last_buf)) return false;

    uint32_t remaining = g_fs.block_size;
    ext2_dir_entry_t* entry = (ext2_dir_entry_t*)last_buf;
    entry->inode = inode_no;
    entry->rec_len = remaining;
    entry->name_len = name_len;
    entry->file_type = file_type;
    memcpy(entry->name, name, name_len);

    uint32_t phys_block;
    if (ext2_get_block_ptr(&updated_inode, block_idx, &phys_block) && phys_block) {
        return write_block(phys_block, last_buf);
    }

    return false;
}

// ============================================
// EXT2 API 实现
// ============================================

bool ext2_mount(uint32_t partition_start)
{
    memset(&g_fs, 0, sizeof(g_fs));
    memset(g_error, 0, sizeof(g_error));
    g_fs.partition_start = partition_start;

    // 读取超级块（位于分区内偏移 1024 字节处 = block 2 的前半部分）
    // 即 LBA = partition_start + (1024 / 512) = partition_start + 2
    uint8_t sector[SECTOR_SIZE];
    if (ide_read_sectors(partition_start + 2, 1, sector) != 0) {
        set_error("Failed to read superblock sector");
        return false;
    }

    // 超级块从扇区内偏移 0 开始（1024 字节对齐 = 2 个扇区）
    memcpy(&g_fs.sb, sector, sizeof(ext2_superblock_t));

    if (g_fs.sb.magic != EXT2_MAGIC) {
        // 尝试从 partition_start + 0 开始读超级块（启动盘模式）
        if (ide_read_sectors(partition_start, 1, sector) == 0) {
            memcpy(&g_fs.sb, sector, sizeof(ext2_superblock_t));
        }

        if (g_fs.sb.magic != EXT2_MAGIC) {
            serial_puts("EXT2: Bad magic (expected 0xEF53, got 0x");
            serial_puthex16(g_fs.sb.magic);
            serial_puts(")\n");
            set_error("Invalid EXT2 superblock magic");
            return false;
        }
    }

    g_fs.block_size = 1024 << g_fs.sb.log_block_size;
    g_fs.blocks_per_group = g_fs.sb.blocks_per_group;
    g_fs.inodes_per_group = g_fs.sb.inodes_per_group;
    g_fs.bg_desc_count = (g_fs.sb.blocks_count + g_fs.blocks_per_group - 1) / g_fs.blocks_per_group;

    // 验证块大小
    if (g_fs.block_size > 4096 || g_fs.block_size < 1024) {
        set_error("Unsupported block size");
        return false;
    }

    serial_puts("EXT2: Mounted successfully\n");
    serial_puts("  Block size: ");
    serial_putdec32(g_fs.block_size);
    serial_puts("\n");
    serial_puts("  Blocks: ");
    serial_putdec32(g_fs.sb.blocks_count);
    serial_puts("\n");
    serial_puts("  Inodes: ");
    serial_putdec32(g_fs.sb.inodes_count);
    serial_puts("\n");
    serial_puts("  Free blocks: ");
    serial_putdec32(g_fs.sb.free_blocks_count);
    serial_puts("\n");
    serial_puts("  Free inodes: ");
    serial_putdec32(g_fs.sb.free_inodes_count);
    serial_puts("\n");

    // 读取 BGDT 信息
    read_bgdt();

    g_mounted = true;
    return true;
}

void ext2_umount(void)
{
    g_mounted = false;
    memset(&g_fs, 0, sizeof(g_fs));
}

// ============================================
// VFS 回调函数（通过 vfs_file_t.fs_private 传递 ext2_handle_t）
// ============================================

static bool ext2_priv_read(void* priv, void* buffer, uint32_t size, uint32_t* bytes_read)
{
    ext2_handle_t* h = (ext2_handle_t*)priv;
    if (!h || !h->is_open) {
        set_error("Handle not open");
        return false;
    }

    if (h->position >= h->file_size) {
        *bytes_read = 0;
        return true; // EOF
    }

    if (h->position + size > h->file_size) {
        size = h->file_size - h->position;
    }

    if (size == 0) {
        *bytes_read = 0;
        return true;
    }

    uint32_t bytes_per_block = g_fs.block_size;
    uint32_t start_block = h->position / bytes_per_block;
    uint32_t end_block = (h->position + size - 1) / bytes_per_block;

    uint32_t offset = h->position % bytes_per_block;
    uint32_t remaining = size;
    uint8_t* dst = (uint8_t*)buffer;

    for (uint32_t b = start_block; b <= end_block; b++) {
        uint8_t block_buf[4096];
        if (!ext2_read_inode_block(&h->inode, b, block_buf)) {
            set_error("Failed to read data block");
            return false;
        }

        uint32_t copy_len = bytes_per_block - offset;
        if (copy_len > remaining) copy_len = remaining;

        memcpy(dst, block_buf + offset, copy_len);

        dst += copy_len;
        remaining -= copy_len;
        h->position += copy_len;
        offset = 0;
    }

    *bytes_read = size;
    return true;
}

static bool ext2_priv_read_dir(void* priv, vfs_dirent_t* ventry)
{
    ext2_handle_t* h = (ext2_handle_t*)priv;
    if (!h || !h->is_open || !h->is_directory) {
        set_error("Not a directory or not open");
        return false;
    }

    uint32_t bytes_per_block = g_fs.block_size;
    uint32_t total_blocks = (h->file_size + bytes_per_block - 1) / bytes_per_block;

    while (h->dir_block_idx < total_blocks) {
        uint8_t block_buf[4096];
        if (!ext2_read_inode_block(&h->inode, h->dir_block_idx, block_buf)) {
            set_error("Failed to read directory block");
            return false;
        }

        while (h->dir_offset < bytes_per_block) {
            ext2_dir_entry_t* entry = (ext2_dir_entry_t*)(block_buf + h->dir_offset);

            if (entry->inode == 0 || entry->rec_len == 0) {
                h->dir_offset = bytes_per_block;
                break;
            }

            uint32_t next_offset = h->dir_offset + entry->rec_len;

            if (entry->name_len == 1 && entry->name[0] == '.') {
                h->dir_offset = next_offset;
                continue;
            }
            if (entry->name_len == 2 && entry->name[0] == '.' && entry->name[1] == '.') {
                h->dir_offset = next_offset;
                continue;
            }

            uint32_t copy_len = entry->name_len;
            if (copy_len >= VFS_MAX_NAME) copy_len = VFS_MAX_NAME - 1;
            memcpy(ventry->name, entry->name, copy_len);
            ventry->name[copy_len] = '\0';

            if (entry->file_type == EXT2_FT_DIR) {
                ventry->is_directory = true;
                ventry->size = 0;
            } else {
                ventry->is_directory = false;
                ext2_inode_t target_inode;
                if (ext2_read_inode(entry->inode, &target_inode)) {
                    ventry->size = target_inode.size;
                } else {
                    ventry->size = 0;
                }
            }
            ventry->mode = 0;

            h->dir_offset = next_offset;
            return true;
        }

        h->dir_block_idx++;
        h->dir_offset = 0;
    }

    return false;
}

static void ext2_priv_close(void* priv)
{
    ext2_handle_t* h = (ext2_handle_t*)priv;
    if (h) {
        if (h->dir_block_data) {
            kfree(h->dir_block_data);
        }
        h->is_open = false;
        kfree(h);
    }
}

// ============================================
// VFS 驱动接口（匹配 fs_driver_t 签名）
// ============================================

bool ext2_open(const char* path, vfs_file_t* vf, vfs_mode_t mode)
{
    if (!path || !vf) {
        set_error("Invalid parameters");
        return false;
    }

    // 分配 EXT2 句柄
    ext2_handle_t* h = (ext2_handle_t*)kmalloc(sizeof(ext2_handle_t));
    if (!h) {
        set_error("Out of memory");
        return false;
    }
    memset(h, 0, sizeof(ext2_handle_t));

    ext2_inode_t inode;
    uint32_t inode_no;

    if (!ext2_find_entry(path, &inode, &inode_no, NULL, NULL)) {
        if (mode == VFS_CREATE) {
            // 需要创建文件
            kfree(h);
            if (!ext2_create_file(path)) return false;

            // 重新打开
            h = (ext2_handle_t*)kmalloc(sizeof(ext2_handle_t));
            if (!h) { set_error("Out of memory"); return false; }
            memset(h, 0, sizeof(ext2_handle_t));

            if (!ext2_find_entry(path, &inode, &inode_no, NULL, NULL)) {
                kfree(h);
                set_error("Failed to open newly created file");
                return false;
            }
        } else {
            kfree(h);
            return false;
        }
    }

    h->inode_no = inode_no;
    memcpy(&h->inode, &inode, sizeof(ext2_inode_t));
    h->position = 0;
    h->file_size = inode.size;
    h->is_directory = EXT2_S_ISDIR(inode.mode);
    h->is_open = true;
    h->dir_block_idx = 0;
    h->dir_block_phys = 0;
    h->dir_offset = 0;
    h->dir_block_data = NULL;

    vf->inode = inode_no;
    vf->file_size = inode.size;
    vf->position = 0;
    vf->is_directory = h->is_directory;
    vf->is_open = true;
    vf->fs_private = h;
    vf->read_fn = ext2_priv_read;
    vf->write_fn = NULL;
    vf->close_fn = ext2_priv_close;
    vf->read_dir_fn = ext2_priv_read_dir;

    return true;
}

// 兼容接口：直接读取到缓冲区（不通过 VFS 回调）
bool ext2_read_all_fast(void* handle, void* buffer)
{
    ext2_handle_t* h = (ext2_handle_t*)handle;
    if (!h || !h->is_open) {
        set_error("Handle not open");
        return false;
    }

    uint32_t bytes_read;
    return ext2_priv_read(h, buffer, h->file_size, &bytes_read);
}

bool ext2_file_exists(const char* path)
{
    ext2_inode_t inode;
    uint32_t ino;
    return ext2_find_entry(path, &inode, &ino, NULL, NULL);
}

bool ext2_create_file(const char* path)
{
    // 检查文件是否已存在
    ext2_inode_t existing;
    uint32_t existing_ino;
    if (ext2_find_entry(path, &existing, &existing_ino, NULL, NULL)) {
        set_error("File already exists");
        return false;
    }

    // 解析路径，找到父目录
    char parent_path[256];
    const char* filename;
    const char* p = path + strlen(path) - 1;

    // 从后往前找最后一个 '/'
    while (p > path && *p != '/') p--;

    if (p == path) {
        // 根目录下的文件
        strcpy(parent_path, "/");
        filename = path + 1;
    } else {
        size_t parent_len = p - path;
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
        filename = p + 1;
    }

    // 获取父目录 inode
    ext2_inode_t parent_inode;
    uint32_t parent_ino;
    if (!ext2_find_entry(parent_path, &parent_inode, &parent_ino, NULL, NULL)) {
        set_error("Parent directory not found");
        return false;
    }

    if (!EXT2_S_ISDIR(parent_inode.mode)) {
        set_error("Parent is not a directory");
        return false;
    }

    // 分配 inode
    uint32_t new_ino = alloc_inode();
    if (new_ino == 0) return false;

    // 初始化新 inode
    ext2_inode_t new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.mode = EXT2_S_IFREG | 0x1FF; // 普通文件，权限 777
    new_inode.uid = 0;
    new_inode.size = 0;
    new_inode.gid = 0;
    new_inode.links_count = 1;
    new_inode.blocks = 0;
    new_inode.atime = 0;
    new_inode.ctime = 0;
    new_inode.mtime = 0;

    if (!ext2_write_inode(new_ino, &new_inode)) return false;

    // 在父目录中添加条目
    if (!ext2_add_dir_entry(parent_ino, &parent_inode, filename, new_ino, EXT2_FT_REG_FILE)) {
        return false;
    }

    return true;
}

bool ext2_create_dir(const char* path)
{
    // 检查是否已存在
    ext2_inode_t existing;
    uint32_t existing_ino;
    if (ext2_find_entry(path, &existing, &existing_ino, NULL, NULL)) {
        set_error("Directory already exists");
        return false;
    }

    // 解析父路径
    char parent_path[256];
    const char* dirname;
    const char* p = path + strlen(path) - 1;

    while (p > path && *p != '/') p--;

    if (p == path) {
        strcpy(parent_path, "/");
        dirname = path + 1;
    } else {
        size_t parent_len = p - path;
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
        dirname = p + 1;
    }

    if (strlen(dirname) == 0) {
        set_error("Invalid directory name");
        return false;
    }

    ext2_inode_t parent_inode;
    uint32_t parent_ino;
    if (!ext2_find_entry(parent_path, &parent_inode, &parent_ino, NULL, NULL)) {
        set_error("Parent directory not found");
        return false;
    }

    if (!EXT2_S_ISDIR(parent_inode.mode)) {
        set_error("Parent is not a directory");
        return false;
    }

    // 分配 inode
    uint32_t new_ino = alloc_inode();
    if (new_ino == 0) return false;

    // 分配一个数据块（用于存放 . 和 .. 条目）
    uint32_t data_block = alloc_block();
    if (data_block == 0) return false;

    // 初始化新 inode
    ext2_inode_t new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.mode = EXT2_S_IFDIR | 0x1FF; // 目录，权限 777
    new_inode.uid = 0;
    new_inode.size = g_fs.block_size;
    new_inode.gid = 0;
    new_inode.links_count = 2; // . 和父目录中的条目
    new_inode.blocks = g_fs.block_size / 512;
    new_inode.block[0] = data_block;
    new_inode.atime = 0;
    new_inode.ctime = 0;
    new_inode.mtime = 0;

    if (!ext2_write_inode(new_ino, &new_inode)) return false;

    // 初始化数据块：添加 . 和 .. 条目
    uint8_t block_buf[4096];
    memset(block_buf, 0, g_fs.block_size);

    // '.' 条目
    ext2_dir_entry_t* dot = (ext2_dir_entry_t*)block_buf;
    dot->inode = new_ino;
    dot->rec_len = 12 + 1; // 最小对齐：12 + 1 = 13，对齐到16
    // 实际上 rec_len 需要覆盖到下一个条目
    // . 名称长度1，条目大小 = 8 + 1 + 1 + 1 = 11 → 对齐到12
    // 所以设 rec_len = 12
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->name[0] = '.';

    // '..' 条目
    ext2_dir_entry_t* dotdot = (ext2_dir_entry_t*)(block_buf + 12);
    dotdot->inode = parent_ino;
    dotdot->rec_len = g_fs.block_size - 12; // 剩余空间
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';

    if (!write_block(data_block, block_buf)) return false;

    // 在父目录中添加条目
    if (!ext2_add_dir_entry(parent_ino, &parent_inode, dirname, new_ino, EXT2_FT_DIR)) {
        return false;
    }

    // 增加父目录的 links_count（.. 增加了链接计数）
    parent_inode.links_count++;
    ext2_write_inode(parent_ino, &parent_inode);

    return true;
}

const char* ext2_get_error(void)
{
    return g_error;
}

bool ext2_mounted(void)
{
    return g_mounted;
}

uint32_t ext2_detect(void)
{
    uint8_t sector[512];

    // 直接读取 LBA 0（MBR），绕过 fscache 的返回值 bug
    if (ide_read_sectors(0, 1, sector) != 0) {
        serial_puts("EXT2 detect: Failed to read LBA 0\n");
        return 0;
    }

    // 检查 MBR 签名
    if (sector[510] == 0x55 && sector[511] == 0xAA) {
        // 有 MBR，遍历分区表
        for (int i = 0; i < 4; i++) {
            uint8_t* p = sector + 446 + i * 16;
            uint8_t type = p[4];

            // EXT2 分区类型 0x83
            if (type == 0x83) {
                uint32_t start_lba = *(uint32_t*)(p + 8);
                serial_puts("EXT2 detect: Found Linux partition (0x83) at LBA ");
                serial_putdec32(start_lba);
                serial_puts("\n");

                // 验证确实是 EXT2
                uint8_t sb_sector[512];
                if (ide_read_sectors(start_lba + 2, 1, sb_sector) == 0) {
                    ext2_superblock_t* sb = (ext2_superblock_t*)sb_sector;
                    if (sb->magic == EXT2_MAGIC) {
                        serial_puts("  -> Confirmed EXT2 magic\n");
                        return start_lba;
                    }
                }
            }
        }
    }

    // 检查是否是 EXT2 超级软盘（没有 MBR，整个磁盘就是 EXT2）
    if (ide_read_sectors(2, 1, sector) == 0) {
        ext2_superblock_t* sb = (ext2_superblock_t*)sector;
        if (sb->magic == EXT2_MAGIC) {
            serial_puts("EXT2 detect: Superfloppy mode (whole disk is EXT2)\n");
            return 0; // 分区从 LBA 0 开始
        }
    }

    serial_puts("EXT2 detect: No EXT2 partition found\n");
    return 0;
}

// VFS 驱动表
static fs_driver_t g_ext2_driver = {
    .mount       = ext2_mount,
    .umount      = ext2_umount,
    .open        = ext2_open,
    .file_exists = ext2_file_exists,
    .create_file = ext2_create_file,
    .create_dir  = ext2_create_dir,
    .get_error   = ext2_get_error,
    .mounted     = ext2_mounted,
};

void ext2_register(void)
{
    vfs_register("ext2", &g_ext2_driver);
    serial_puts("EXT2: Registered with VFS\n");
}
