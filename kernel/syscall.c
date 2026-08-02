#include "kernel.h"
#include "serial.h"
#include "idt.h"
#include "trap.h"
#include "gdt.h"
#include "elfexev.h"
#include "syscall_nr.h"
#include "drivers/fs/vfs.h"
#include "klog.h"
#include "memory.h"
#include "string.h"

/* ======================================================================== */
/*  Syscall kernel stack — used by syscall_entry (interrupt_stub.asm)      */
/* ======================================================================== */
static uint8_t g_syscall_kernel_stack_buf[4096] __attribute__((aligned(16)));
uint64_t g_syscall_kernel_stack = (uint64_t)(g_syscall_kernel_stack_buf + 4096 - 16);
uint64_t g_saved_user_rsp = 0;

/* 用户程序退出标志 – 在 syscall_entry 汇编中检查 */
int g_exit_program = 0;

/* ======================================================================== */
/*  File descriptor table                                                   */
/* ======================================================================== */
#define MAX_FD 32

typedef struct {
    vfs_file_t file;       /* VFS file descriptor (if is_vfs) */
    bool       used;
    bool       is_vfs;     /* true = real VFS file; false = virtual (stdin/out/err) */
    int        flags;      /* O_RDONLY / O_WRONLY / O_RDWR */
} fd_entry_t;

static fd_entry_t g_fd_table[MAX_FD];
static bool g_fd_initialized = false;

static void fd_table_init(void)
{
    if (g_fd_initialized) return;
    for (int i = 0; i < MAX_FD; i++)
        g_fd_table[i].used = false;

    /* fd 0=stdin, 1=stdout, 2=stderr — 虚拟 fd */
    g_fd_table[0].used     = true;
    g_fd_table[0].is_vfs   = false;
    g_fd_table[0].flags    = O_RDONLY;

    g_fd_table[1].used     = true;
    g_fd_table[1].is_vfs   = false;
    g_fd_table[1].flags    = O_WRONLY;

    g_fd_table[2].used     = true;
    g_fd_table[2].is_vfs   = false;
    g_fd_table[2].flags    = O_WRONLY;

    g_fd_initialized = true;
}

static int fd_alloc(void)
{
    for (int i = 3; i < MAX_FD; i++) {
        if (!g_fd_table[i].used) {
            g_fd_table[i].used = true;
            g_fd_table[i].is_vfs = false;
            return i;
        }
    }
    return -EMFILE;
}

static void fd_free(int fd)
{
    if (fd < 3 || fd >= MAX_FD) return;
    if (g_fd_table[fd].used && g_fd_table[fd].is_vfs)
        vfs_close(&g_fd_table[fd].file);
    g_fd_table[fd].used = false;
}

/* ======================================================================== */
/*  Heap (brk) management                                                   */
/* ======================================================================== */
#define HEAP_START  0x700000ULL
#define HEAP_MAX    0x10000000ULL    /* 256 MB */

static uint64_t g_heap_brk = HEAP_START;
static uint64_t g_heap_mapped = HEAP_START;  /* 已映射到的位置 */

static long sys_brk(uint64_t addr)
{
    if (addr == 0)
        return (long)g_heap_brk;  /* 查询当前 brk */

    if (addr < HEAP_START || addr > HEAP_MAX)
        return -ENOMEM;

    /* 扩展堆：映射新页面 */
    while (g_heap_mapped < addr) {
        void *phys = pmm_alloc_zpage();
        if (!phys) return -ENOMEM;
        vmm_map_user(kernel_pml4, g_heap_mapped, (uint64_t)phys);
        vmm_make_user(kernel_pml4, g_heap_mapped);
        g_heap_mapped += PAGE_SIZE;
    }

    /* 收缩堆：取消映射 */
    while (g_heap_mapped > addr + PAGE_SIZE) {
        /* 简单起见，暂不取消映射，只移动 brk 指针 */
        break;
    }

    g_heap_brk = addr;
    return (long)g_heap_brk;
}

/* ======================================================================== */
/*  mmap / munmap 简化实现（仅匿名映射）                                     */
/* ======================================================================== */
static uint64_t g_mmap_next = 0x10000000ULL;  /* 下一个匿名映射地址 */

static long sys_mmap(uint64_t addr, uint64_t length, int prot,
                     int flags, int fd, int64_t offset)
{
    (void)prot;
    (void)fd;
    (void)offset;

    /* 仅支持匿名映射 */
    if (!(flags & MAP_ANONYMOUS))
        return -ENOSYS;

    /* 如果 addr 为 0，自动分配 */
    if (addr == 0)
        addr = g_mmap_next;

    uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t aligned = addr & ~0xFFFULL;

    for (uint64_t p = aligned; p < aligned + pages * PAGE_SIZE; p += PAGE_SIZE) {
        void *phys = pmm_alloc_zpage();
        if (!phys) return -ENOMEM;
        vmm_map_user(kernel_pml4, p, (uint64_t)phys);
        vmm_make_user(kernel_pml4, p);
    }

    if (addr + pages * PAGE_SIZE > g_mmap_next)
        g_mmap_next = addr + pages * PAGE_SIZE;

    return (long)aligned;
}

static long sys_munmap(uint64_t addr, uint64_t length)
{
    /* 简化实现：暂不取消映射，只返回成功 */
    (void)addr;
    (void)length;
    return 0;
}

static long sys_mprotect(uint64_t addr, uint64_t len, int prot)
{
    (void)addr;
    (void)len;
    (void)prot;
    return 0;  /* stub */
}

/* ======================================================================== */
/*  read / write 系统调用                                                    */
/* ======================================================================== */
static long sys_read(int fd, uint64_t buf, size_t count)
{
    if (fd < 0 || fd >= MAX_FD || !g_fd_table[fd].used)
        return -EBADF;

    if (fd == 0) {
        /* stdin — 当前无输入，返回 0（EOF） */
        (void)buf;
        (void)count;
        return 0;
    }

    /* VFS 文件读取 */
    if (g_fd_table[fd].is_vfs) {
        uint32_t bytes_read = 0;
        if (!vfs_read(&g_fd_table[fd].file, (void*)buf, (uint32_t)count, &bytes_read))
            return -EIO;
        return (long)bytes_read;
    }

    return -EBADF;
}

static long sys_write(int fd, uint64_t buf, size_t count)
{
    if (fd < 0 || fd >= MAX_FD || !g_fd_table[fd].used)
        return -EBADF;

    if (fd == 1 || fd == 2) {
        /* stdout / stderr → 串口输出 */
        for (size_t i = 0; i < count; i++)
            serial_putc(((char*)buf)[i]);
        return (long)count;
    }

    /* VFS 文件写入 */
    if (g_fd_table[fd].is_vfs) {
        uint32_t bytes_written = 0;
        if (!vfs_write(&g_fd_table[fd].file, (void*)buf, (uint32_t)count, &bytes_written))
            return -EIO;
        return (long)bytes_written;
    }

    return -EBADF;
}

/* ======================================================================== */
/*  open / close 系统调用                                                    */
/* ======================================================================== */
static long sys_open(const char *path, int flags, int mode)
{
    (void)mode;

    /* 将标志转换为 VFS 模式 */
    vfs_mode_t vfs_mode;
    if ((flags & O_RDWR) == O_RDWR)
        vfs_mode = VFS_READ;  /* VFS 不支持同时读写，暂用读 */
    else if (flags & O_WRONLY)
        vfs_mode = VFS_WRITE;
    else
        vfs_mode = VFS_READ;

    int fd = fd_alloc();
    if (fd < 0) return fd;

    if (flags & O_CREAT) {
        if (!vfs_file_exists(path)) {
            if (!vfs_create_file(path)) {
                fd_free(fd);
                return -ENOENT;
            }
        }
    }

    if (!vfs_open(path, &g_fd_table[fd].file, vfs_mode)) {
        fd_free(fd);
        return -ENOENT;
    }

    g_fd_table[fd].is_vfs = true;
    g_fd_table[fd].flags  = flags;
    return (long)fd;
}

static long sys_close(int fd)
{
    if (fd < 0 || fd >= MAX_FD || !g_fd_table[fd].used)
        return -EBADF;
    if (fd < 3) return 0;  /* 不能关闭 stdin/stdout/stderr */
    fd_free(fd);
    return 0;
}

/* ======================================================================== */
/*  stat / fstat / lseek / getdents64                                       */
/* ======================================================================== */
static long sys_fstat(int fd, struct stat *st)
{
    if (fd < 0 || fd >= MAX_FD || !g_fd_table[fd].used)
        return -EBADF;

    memset(st, 0, sizeof(struct stat));

    if (fd == 1 || fd == 2) {
        /* stdout/stderr — 模拟一个 tty */
        st->st_mode = 0x2190;  /* S_IFCHR | 0620 */
        st->st_size = 0;
        return 0;
    }
    if (fd == 0) {
        st->st_mode = 0x2190;
        st->st_size = 0;
        return 0;
    }

    if (g_fd_table[fd].is_vfs) {
        st->st_dev   = 0;
        st->st_ino   = g_fd_table[fd].file.inode;
        st->st_mode  = g_fd_table[fd].file.is_directory ? 0x41ED : 0x81A4;
        st->st_size  = g_fd_table[fd].file.file_size;
        st->st_blksize = 512;
        st->st_blocks  = (st->st_size + 511) / 512;
        st->st_nlink = 1;
        return 0;
    }

    return -EBADF;
}

static long sys_stat(const char *path, struct stat *st)
{
    vfs_file_t file;
    if (!vfs_open(path, &file, VFS_READ))
        return -ENOENT;

    memset(st, 0, sizeof(struct stat));
    st->st_dev   = 0;
    st->st_ino   = file.inode;
    st->st_mode  = file.is_directory ? 0x41ED : 0x81A4;
    st->st_size  = file.file_size;
    st->st_blksize = 512;
    st->st_blocks  = (st->st_size + 511) / 512;
    st->st_nlink = 1;

    vfs_close(&file);
    return 0;
}

static long sys_lseek(int fd, int64_t offset, int whence)
{
    if (fd < 0 || fd >= MAX_FD || !g_fd_table[fd].used)
        return -EBADF;
    if (!g_fd_table[fd].is_vfs)
        return -ESPIPE;

    uint32_t new_pos;
    switch (whence) {
    case SEEK_SET:
        new_pos = (uint32_t)offset;
        break;
    case SEEK_CUR:
        new_pos = g_fd_table[fd].file.position + (uint32_t)offset;
        break;
    case SEEK_END:
        new_pos = g_fd_table[fd].file.file_size + (uint32_t)offset;
        break;
    default:
        return -EINVAL;
    }
    g_fd_table[fd].file.position = new_pos;
    return (long)new_pos;
}

static long sys_getdents64(int fd, struct dirent64 *dirp, size_t count)
{
    if (fd < 0 || fd >= MAX_FD || !g_fd_table[fd].used)
        return -EBADF;
    if (!g_fd_table[fd].is_vfs)
        return -EBADF;

    vfs_dirent_t entry;
    size_t total = 0;
    while (total + sizeof(struct dirent64) + 1 <= count) {
        if (!vfs_read_dir(&g_fd_table[fd].file, &entry))
            break;

        size_t namelen = strlen(entry.name);
        size_t reclen = sizeof(struct dirent64) + namelen + 1;
        if (reclen > count - total) break;

        struct dirent64 *d = (struct dirent64 *)((uint8_t*)dirp + total);
        d->d_ino    = 0;
        d->d_off    = 0;
        d->d_reclen = (unsigned short)reclen;
        d->d_type   = entry.is_directory ? 4 : 8;
        memcpy(d->d_name, entry.name, namelen + 1);
        total += reclen;
    }
    return (long)total;
}

/* ======================================================================== */
/*  readv / writev                                                          */
/* ======================================================================== */
static long sys_readv(int fd, const struct iovec *iov, int iovcnt)
{
    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        long r = sys_read(fd, (uint64_t)iov[i].iov_base, iov[i].iov_len);
        if (r < 0) return (total > 0) ? total : r;
        total += r;
    }
    return total;
}

static long sys_writev(int fd, const struct iovec *iov, int iovcnt)
{
    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        long r = sys_write(fd, (uint64_t)iov[i].iov_base, iov[i].iov_len);
        if (r < 0) return (total > 0) ? total : r;
        total += r;
    }
    return total;
}

/* ======================================================================== */
/*  dup / dup2                                                              */
/* ======================================================================== */
static long sys_dup(int oldfd)
{
    if (oldfd < 0 || oldfd >= MAX_FD || !g_fd_table[oldfd].used)
        return -EBADF;
    int newfd = fd_alloc();
    if (newfd < 0) return newfd;
    g_fd_table[newfd] = g_fd_table[oldfd];
    if (g_fd_table[newfd].is_vfs) {
        /* 重新打开一次以共享文件位置（简化处理） */
    }
    return (long)newfd;
}

static long sys_dup2(int oldfd, int newfd)
{
    if (oldfd < 0 || oldfd >= MAX_FD || !g_fd_table[oldfd].used)
        return -EBADF;
    if (newfd < 0 || newfd >= MAX_FD)
        return -EBADF;
    if (newfd > 2 && g_fd_table[newfd].used)
        fd_free(newfd);
    g_fd_table[newfd] = g_fd_table[oldfd];
    g_fd_table[newfd].used = true;
    return (long)newfd;
}

/* ======================================================================== */
/*  access / getcwd / chdir / mkdir / rmdir / unlink / creat                */
/* ======================================================================== */
static long sys_access(const char *path, int mode)
{
    (void)mode;
    if (vfs_file_exists(path))
        return 0;
    return -ENOENT;
}

static long sys_getcwd(char *buf, size_t size)
{
    if (size < 2) return -ERANGE;
    buf[0] = '/';
    buf[1] = '\0';
    return 1;
}

static long sys_chdir(const char *path)
{
    if (!vfs_file_exists(path))
        return -ENOENT;
    return 0;
}

static long sys_mkdir(const char *path, int mode)
{
    (void)mode;
    if (vfs_create_dir(path))
        return 0;
    return -ENOENT;
}

static long sys_rmdir(const char *path)
{
    /* VFS 暂无删除目录接口，直接返回成功 */
    (void)path;
    return 0;
}

static long sys_unlink(const char *path)
{
    /* VFS 暂无删除文件接口，直接返回成功 */
    (void)path;
    return 0;
}

static long sys_creat(const char *path, int mode)
{
    (void)mode;
    return sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0);
}

/* ======================================================================== */
/*  nanosleep / clock_gettime / gettimeofday / time                         */
/* ======================================================================== */
static long sys_nanosleep(const struct timespec *req, struct timespec *rem)
{
    /* 基于 PIT 的简单忙等待（1000 Hz = 1ms 精度） */
    uint64_t total_ms = req->tv_sec * 1000 + req->tv_nsec / 1000000;
    uint64_t start = timer_get_ticks();

    while (timer_get_ticks() - start < total_ms) {
        asm volatile("pause");
    }

    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

static long sys_clock_gettime(int clk_id, struct timespec *tp)
{
    (void)clk_id;
    /* 从 PIT 获取大致时间 */
    uint64_t ticks = timer_get_ticks();
    tp->tv_sec  = ticks / 1000;
    tp->tv_nsec = (ticks % 1000) * 1000000;
    return 0;
}

static long sys_gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    uint64_t ticks = timer_get_ticks();
    if (tv) {
        tv->tv_sec  = ticks / 1000;
        tv->tv_usec = (ticks % 1000) * 1000;
    }
    return 0;
}

static long sys_time(uint64_t *tloc)
{
    uint64_t t = timer_get_ticks() / 1000;
    if (tloc) *tloc = t;
    return (long)t;
}

/* ======================================================================== */
/*  getpid / getuid / getgid / geteuid / getegid / getppid                  */
/* ======================================================================== */
static long sys_getpid(void)        { return 1; }
static long sys_getuid(void)        { return 0; }
static long sys_getgid(void)        { return 0; }
static long sys_geteuid(void)       { return 0; }
static long sys_getegid(void)       { return 0; }
static long sys_getppid(void)       { return 0; }
static long sys_getpgrp(void)       { return 0; }
static long sys_setsid(void)        { return 0; }
static long sys_sched_yield(void)   { return 0; }
static long sys_sched_setparam(long pid, const void *param)        { (void)pid; (void)param; return 0; }
static long sys_sched_getparam(long pid, void *param)              { (void)pid; (void)param; return 0; }
static long sys_sched_getscheduler(long pid)                       { (void)pid; return 0; }
static long sys_sched_setscheduler(long pid, int policy, const void *param) { (void)pid; (void)policy; (void)param; return 0; }
static long sys_sched_get_priority_max(int policy)                 { (void)policy; return 0; }
static long sys_sched_get_priority_min(int policy)                 { (void)policy; return 0; }
static long sys_sched_rr_get_interval(long pid, struct timespec *tp) { (void)pid; (void)tp; return 0; }

/* ======================================================================== */
/*  uname                                                                   */
/* ======================================================================== */
static long sys_uname(struct utsname *buf)
{
    if (!buf) return -EFAULT;
    memset(buf, 0, sizeof(struct utsname));
    memcpy(buf->sysname,  "MWOS",   5);
    memcpy(buf->nodename, "mwos",   5);
    memcpy(buf->release,  "0.1.0",  6);
    memcpy(buf->version,  "MWOS",   5);
    memcpy(buf->machine,  "x86_64", 7);
    return 0;
}

/* ======================================================================== */
/*  ioctl                                                                   */
/* ======================================================================== */
static long sys_ioctl(int fd, unsigned long request, uint64_t arg)
{
    (void)fd;
    (void)request;
    (void)arg;
    return -ENOTTY;
}

/* ======================================================================== */
/*  fcntl                                                                   */
/* ======================================================================== */
static long sys_fcntl(int fd, int cmd, uint64_t arg)
{
    if (fd < 0 || fd >= MAX_FD || !g_fd_table[fd].used)
        return -EBADF;

    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        return sys_dup(fd);
    case F_GETFD:
        return 0;
    case F_SETFD:
        return 0;
    case F_GETFL:
        return g_fd_table[fd].flags;
    case F_SETFL:
        g_fd_table[fd].flags = (int)arg;
        return 0;
    default:
        return -EINVAL;
    }
}

/* ======================================================================== */
/*  futex — stub (单线程)                                                   */
/* ======================================================================== */
static long sys_futex(uint64_t uaddr, int op, uint32_t val,
                      const struct timespec *timeout, uint64_t uaddr2, uint32_t val3)
{
    (void)uaddr;
    (void)op;
    (void)val;
    (void)timeout;
    (void)uaddr2;
    (void)val3;
    /* 单线程环境，futex 唤醒/等待都返回 0 或 -ENOSYS */
    if (op == 0 || op == 1)  /* FUTEX_WAIT / FUTEX_WAKE */
        return 0;
    return -ENOSYS;
}

/* ======================================================================== */
/*  pipe — stub                                                             */
/* ======================================================================== */
static long sys_pipe(int *pipefd)
{
    (void)pipefd;
    return -ENOSYS;
}

/* ======================================================================== */
/*  wait4 — stub                                                            */
/* ======================================================================== */
static long sys_wait4(int pid, int *status, int options, struct rusage *rusage)
{
    (void)pid;
    (void)status;
    (void)options;
    (void)rusage;
    return -ECHILD;
}

/* ======================================================================== */
/*  kill / tkill / tgkill — stub                                            */
/* ======================================================================== */
static long sys_kill(int pid, int sig)         { (void)pid; (void)sig; return 0; }
static long sys_tkill(int tid, int sig)         { (void)tid; (void)sig; return 0; }
static long sys_tgkill(int tgid, int tid, int sig) { (void)tgid; (void)tid; (void)sig; return 0; }

/* ======================================================================== */
/*  sigaction / sigprocmask / sigreturn / sigpending / sigaltstack — stub   */
/* ======================================================================== */
static long sys_rt_sigaction(int sig, const struct sigaction *act,
                             struct sigaction *oldact, size_t sigsetsize)
{
    (void)sig;
    (void)act;
    (void)oldact;
    (void)sigsetsize;
    return 0;  /* 忽略所有信号 */
}

static long sys_rt_sigprocmask(int how, const sigset_t *set, sigset_t *oldset, size_t sigsetsize)
{
    (void)how;
    (void)set;
    (void)oldset;
    (void)sigsetsize;
    return 0;
}

static long sys_rt_sigreturn(void)
{
    return 0;
}

static long sys_rt_sigpending(sigset_t *set, size_t sigsetsize)
{
    (void)set;
    (void)sigsetsize;
    return 0;
}

static long sys_rt_sigtimedwait(const sigset_t *set, void *info,
                                const struct timespec *timeout, size_t sigsetsize)
{
    (void)set;
    (void)info;
    (void)timeout;
    (void)sigsetsize;
    return -EAGAIN;
}

static long sys_rt_sigsuspend(const sigset_t *mask, size_t sigsetsize)
{
    (void)mask;
    (void)sigsetsize;
    return -EINTR;
}

static long sys_sigaltstack(const void *ss, void *old_ss)
{
    (void)ss;
    (void)old_ss;
    return 0;
}

/* ======================================================================== */
/*  clone — stub (单线程，不支持 fork)                                      */
/* ======================================================================== */
static long sys_clone(unsigned long flags, uint64_t child_stack,
                      uint64_t parent_tid, uint64_t child_tid, unsigned long tls)
{
    (void)flags;
    (void)child_stack;
    (void)parent_tid;
    (void)child_tid;
    (void)tls;
    return -ENOSYS;
}

/* ======================================================================== */
/*  set_tid_address / set_robust_list / get_robust_list                     */
/* ======================================================================== */
static long sys_set_tid_address(uint64_t tidptr)
{
    (void)tidptr;
    return 1;  /* 返回当前线程 TID */
}

static long sys_set_robust_list(uint64_t head, size_t len)
{
    (void)head;
    (void)len;
    return 0;
}

static long sys_get_robust_list(int pid, uint64_t *head_ptr, size_t *len_ptr)
{
    (void)pid;
    (void)head_ptr;
    (void)len_ptr;
    return 0;
}

/* ======================================================================== */
/*  arch_prctl — FS base 设置 (用于 TLS)                                    */
/* ======================================================================== */
static long sys_arch_prctl(int code, uint64_t addr)
{
    switch (code) {
    case ARCH_SET_FS: {
        /* 设置 FS.base MSR */
        uint32_t eax = (uint32_t)addr;
        uint32_t edx = (uint32_t)(addr >> 32);
        asm volatile("wrmsr" : : "c"(0xC0000100), "a"(eax), "d"(edx));
        return 0;
    }
    case ARCH_GET_FS: {
        uint32_t eax, edx;
        asm volatile("rdmsr" : "=a"(eax), "=d"(edx) : "c"(0xC0000100));
        uint64_t val = ((uint64_t)edx << 32) | eax;
        *(uint64_t*)addr = val;
        return 0;
    }
    default:
        return -EINVAL;
    }
}

/* ======================================================================== */
/*  exit / exit_group                                                       */
/* ======================================================================== */
static long sys_exit(Trapframe *tf, int error_code)
{
    (void)error_code;
    serial_puts("syscall: exit\n");

    /* 设置退出标志，让 syscall_entry 汇编改用 iretq 返回内核 */
    g_exit_program = 1;

    /* 将 Trapframe 重定向回内核模式 elf_exit_handler */
    extern void elf_exit_handler(void);
    tf->rip     = (uint64_t)elf_exit_handler;
    tf->cs      = SEL_KCODE;
    tf->rflags  = 0x202;        /* IF=1 */
    tf->rsp     = g_elf_ret_rsp; /* 内核栈 */
    tf->ss      = SEL_KDATA;

    return 0;  /* 不会被实际用到 */
}

/* ======================================================================== */
/*  getrlimit / setrlimit — stub                                            */
/* ======================================================================== */
static long sys_getrlimit(int resource, struct rlimit *rlim)
{
    if (!rlim) return -EFAULT;
    switch (resource) {
    case RLIMIT_NOFILE:
        rlim->rlim_cur = MAX_FD;
        rlim->rlim_max = MAX_FD;
        return 0;
    case RLIMIT_STACK:
        rlim->rlim_cur = 16 * 1024 * 1024;  /* 16 MB */
        rlim->rlim_max = 16 * 1024 * 1024;
        return 0;
    case RLIMIT_AS:
        rlim->rlim_cur = 1024 * 1024 * 1024;  /* 1 GB */
        rlim->rlim_max = 1024 * 1024 * 1024;
        return 0;
    default:
        rlim->rlim_cur = 0xFFFFFFFFFFFFFFFFULL;
        rlim->rlim_max = 0xFFFFFFFFFFFFFFFFULL;
        return 0;
    }
}

static long sys_setrlimit(int resource, const struct rlimit *rlim)
{
    (void)resource;
    (void)rlim;
    return 0;
}

/* ======================================================================== */
/*  getrusage — stub                                                        */
/* ======================================================================== */
struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss;
    /* 省略其余字段 */
};
static long sys_getrusage(int who, struct rusage *usage)
{
    (void)who;
    if (usage) memset(usage, 0, sizeof(struct rusage));
    return 0;
}

/* ======================================================================== */
/*  umask — stub                                                            */
/* ======================================================================== */
static long sys_umask(int mask)
{
    (void)mask;
    return 022;
}

/* ======================================================================== */
/*  prctl — stub                                                            */
/* ======================================================================== */
static long sys_prctl(int option, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)option;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    return 0;
}

/* ======================================================================== */
/*  personality — stub                                                      */
/* ======================================================================== */
static long sys_personality(unsigned long persona)
{
    (void)persona;
    return 0;
}

/* ======================================================================== */
/*  socket / connect / bind / listen / accept — stub                        */
/* ======================================================================== */
static long sys_socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;
    return -EAFNOSUPPORT;
}
static long sys_connect(int fd, const struct sockaddr *addr, uint64_t addrlen)
{
    (void)fd;
    (void)addr;
    (void)addrlen;
    return -EAFNOSUPPORT;
}
static long sys_bind(int fd, const struct sockaddr *addr, uint64_t addrlen)
{
    (void)fd;
    (void)addr;
    (void)addrlen;
    return -EAFNOSUPPORT;
}
static long sys_listen(int fd, int backlog)
{
    (void)fd;
    (void)backlog;
    return -EAFNOSUPPORT;
}
static long sys_accept(int fd, struct sockaddr *addr, uint64_t *addrlen)
{
    (void)fd;
    (void)addr;
    (void)addrlen;
    return -EAFNOSUPPORT;
}
static long sys_setsockopt(int fd, int level, int optname, const void *optval, uint64_t optlen)
{
    (void)fd;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    return -EOPNOTSUPP;
}
static long sys_getsockopt(int fd, int level, int optname, void *optval, uint64_t *optlen)
{
    (void)fd;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    return -EOPNOTSUPP;
}
static long sys_sendto(int fd, const void *buf, size_t len, int flags,
                       const struct sockaddr *dest_addr, uint64_t addrlen)
{
    (void)fd;
    (void)buf;
    (void)len;
    (void)flags;
    (void)dest_addr;
    (void)addrlen;
    return -EAFNOSUPPORT;
}
static long sys_recvfrom(int fd, void *buf, size_t len, int flags,
                         struct sockaddr *src_addr, uint64_t *addrlen)
{
    (void)fd;
    (void)buf;
    (void)len;
    (void)flags;
    (void)src_addr;
    (void)addrlen;
    return -EAFNOSUPPORT;
}
static long sys_shutdown(int fd, int how)
{
    (void)fd;
    (void)how;
    return -EAFNOSUPPORT;
}
static long sys_sendmsg(int fd, const void *msg, int flags)
{
    (void)fd;
    (void)msg;
    (void)flags;
    return -EAFNOSUPPORT;
}
static long sys_recvmsg(int fd, void *msg, int flags)
{
    (void)fd;
    (void)msg;
    (void)flags;
    return -EAFNOSUPPORT;
}
static long sys_getsockname(int fd, struct sockaddr *addr, uint64_t *addrlen)
{
    (void)fd;
    (void)addr;
    (void)addrlen;
    return -EAFNOSUPPORT;
}
static long sys_getpeername(int fd, struct sockaddr *addr, uint64_t *addrlen)
{
    (void)fd;
    (void)addr;
    (void)addrlen;
    return -EAFNOSUPPORT;
}
static long sys_socketpair(int domain, int type, int protocol, int *sv)
{
    (void)domain;
    (void)type;
    (void)protocol;
    (void)sv;
    return -EAFNOSUPPORT;
}

/* ======================================================================== */
/*  sendfile — stub                                                         */
/* ======================================================================== */
static long sys_sendfile(int out_fd, int in_fd, int64_t *offset, size_t count)
{
    (void)out_fd;
    (void)in_fd;
    (void)offset;
    (void)count;
    return -ENOSYS;
}

/* ======================================================================== */
/*  execve — stub                                                           */
/* ======================================================================== */
static long sys_execve(const char *path, char *const argv[], char *const envp[])
{
    (void)argv;
    (void)envp;
    /* 调用内核 ELF 加载器 */
    if (elfexec(NULL, path, NULL, NULL) == 0)
        return 0;
    return -ENOENT;
}

/* ======================================================================== */
/*  Dup and other simple stubs                                              */
/* ======================================================================== */
static long sys_poll(struct pollfd *fds, unsigned long nfds, int timeout)
{
    (void)fds;
    (void)nfds;
    (void)timeout;
    return 0;  /* 什么都没有就绪 */
}

static long sys_ppoll(struct pollfd *fds, unsigned long nfds,
                      const struct timespec *timeout, const sigset_t *sigmask, size_t sigsetsize)
{
    (void)fds;
    (void)nfds;
    (void)timeout;
    (void)sigmask;
    (void)sigsetsize;
    return 0;
}

static long sys_select(int nfds, void *readfds, void *writefds,
                       void *exceptfds, struct timeval *timeout)
{
    (void)nfds;
    (void)readfds;
    (void)writefds;
    (void)exceptfds;
    (void)timeout;
    return 0;
}

static long sys_pselect6(int nfds, void *readfds, void *writefds,
                         void *exceptfds, const struct timespec *timeout,
                         const void *sigmask)
{
    (void)nfds;
    (void)readfds;
    (void)writefds;
    (void)exceptfds;
    (void)timeout;
    (void)sigmask;
    return 0;
}

static long sys_gettid(void)          { return 1; }
static long sys_getpgid(int pid)      { (void)pid; return 0; }
static long sys_setpgid(int pid, int pgid) { (void)pid; (void)pgid; return 0; }
static long sys_getsid(int pid)       { (void)pid; return 0; }
static long sys_getpriority(int which, int who) { (void)which; (void)who; return 0; }
static long sys_setpriority(int which, int who, int prio) { (void)which; (void)who; (void)prio; return 0; }
static long sys_getresuid(uint64_t *ruid, uint64_t *euid, uint64_t *suid) { if (ruid) *ruid=0; if (euid) *euid=0; if (suid) *suid=0; return 0; }
static long sys_getresgid(uint64_t *rgid, uint64_t *egid, uint64_t *sgid) { if (rgid) *rgid=0; if (egid) *egid=0; if (sgid) *sgid=0; return 0; }
static long sys_setresuid(uint64_t ruid, uint64_t euid, uint64_t suid) { (void)ruid; (void)euid; (void)suid; return 0; }
static long sys_setresgid(uint64_t rgid, uint64_t egid, uint64_t sgid) { (void)rgid; (void)egid; (void)sgid; return 0; }
static long sys_setreuid(uint64_t ruid, uint64_t euid) { (void)ruid; (void)euid; return 0; }
static long sys_setregid(uint64_t rgid, uint64_t egid) { (void)rgid; (void)egid; return 0; }
static long sys_getgroups(int size, uint64_t *list) { (void)size; (void)list; return 0; }
static long sys_setgroups(int size, const uint64_t *list) { (void)size; (void)list; return 0; }
static long sys_setfsuid(uint64_t uid) { (void)uid; return 0; }
static long sys_setfsgid(uint64_t gid) { (void)gid; return 0; }
static long sys_getuid16(void) { return 0; }
static long sys_geteuid16(void) { return 0; }
static long sys_getgid16(void) { return 0; }
static long sys_getegid16(void) { return 0; }

/* ======================================================================== */
/*  Main syscall handler                                                    */
/* ======================================================================== */

void handle_syscall(Trapframe *tf)
{
    uint64_t nr = tf->rax;
    uint64_t a1 = tf->rdi;
    uint64_t a2 = tf->rsi;
    uint64_t a3 = tf->rdx;
    uint64_t a4 = tf->r10;
    uint64_t a5 = tf->r8;
    uint64_t a6 = tf->r9;
    long ret = 0;

    /* debug: 打印 syscall 入口信息 */
    serial_puts("[syscall] nr=");
    serial_putdec64(nr);
    serial_puts(" user_rip=");
    serial_puthex64(tf->rip);
    serial_puts(" user_cs=");
    serial_puthex64(tf->cs);
    serial_puts(" user_rsp=");
    serial_puthex64(tf->rsp);
    serial_puts(" user_ss=");
    serial_puthex64(tf->ss);
    serial_puts("\n");

    /* 初始化 fd 表（首次调用时） */
    if (!g_fd_initialized)
        fd_table_init();

    switch (nr) {
    /* ========== Core I/O ========== */
    case SYS_read:        ret = sys_read((int)a1, a2, (size_t)a3); break;
    case SYS_write:       ret = sys_write((int)a1, a2, (size_t)a3); break;
    case SYS_open:        ret = sys_open((const char*)a1, (int)a2, (int)a3); break;
    case SYS_close:       ret = sys_close((int)a1); break;
    case SYS_lseek:       ret = sys_lseek((int)a1, (int64_t)a2, (int)a3); break;

    /* ========== File status ========== */
    case SYS_stat:        ret = sys_stat((const char*)a1, (struct stat*)a2); break;
    case SYS_fstat:       ret = sys_fstat((int)a1, (struct stat*)a2); break;
    case SYS_newfstatat:  ret = sys_stat((const char*)a2, (struct stat*)a3); break;
    case SYS_lstat:       ret = sys_stat((const char*)a1, (struct stat*)a2); break;

    /* ========== Memory ========== */
    case SYS_brk:         ret = sys_brk(a1); break;
    case SYS_mmap:        ret = sys_mmap(a1, a2, (int)a3, (int)a4, (int)a5, (int64_t)a6); break;
    case SYS_munmap:      ret = sys_munmap(a1, a2); break;
    case SYS_mprotect:    ret = sys_mprotect(a1, a2, (int)a3); break;
    case SYS_mremap:      ret = 0; break;
    case SYS_madvise:     ret = 0; break;
    case SYS_mlock:       ret = 0; break;
    case SYS_munlock:     ret = 0; break;
    case SYS_mlockall:    ret = 0; break;
    case SYS_munlockall:  ret = 0; break;
    case SYS_mincore:     ret = -ENOSYS; break;
    case SYS_msync:       ret = 0; break;

    /* ========== Process ========== */
    case SYS_exit:        ret = sys_exit(tf, (int)a1); break;
    case SYS_exit_group:  ret = sys_exit(tf, (int)a1); break;
    case SYS_getpid:      ret = sys_getpid(); break;
    case SYS_getppid:     ret = sys_getppid(); break;
    case SYS_getpgrp:     ret = sys_getpgrp(); break;
    case SYS_getpgid:     ret = sys_getpgid((int)a1); break;
    case SYS_setpgid:     ret = sys_setpgid((int)a1, (int)a2); break;
    case SYS_setsid:      ret = sys_setsid(); break;
    case SYS_getsid:      ret = sys_getsid((int)a1); break;
    case SYS_clone:       ret = sys_clone((unsigned long)a1, a2, a3, a4, (unsigned long)a5); break;
    case SYS_fork:        ret = -ENOSYS; break;
    case SYS_vfork:       ret = -ENOSYS; break;
    case SYS_execve:      ret = sys_execve((const char*)a1, (char*const*)a2, (char*const*)a3); break;
    case SYS_wait4:       ret = sys_wait4((int)a1, (int*)a2, (int)a3, (struct rusage*)a4); break;
    case SYS_gettid:      ret = sys_gettid(); break;

    /* ========== UID/GID ========== */
    case SYS_getuid:      ret = sys_getuid(); break;
    case SYS_getgid:      ret = sys_getgid(); break;
    case SYS_geteuid:     ret = sys_geteuid(); break;
    case SYS_getegid:     ret = sys_getegid(); break;
    case SYS_getresuid:   ret = sys_getresuid((uint64_t*)a1, (uint64_t*)a2, (uint64_t*)a3); break;
    case SYS_getresgid:   ret = sys_getresgid((uint64_t*)a1, (uint64_t*)a2, (uint64_t*)a3); break;
    case SYS_setresuid:   ret = sys_setresuid(a1, a2, a3); break;
    case SYS_setresgid:   ret = sys_setresgid(a1, a2, a3); break;
    case SYS_setreuid:    ret = sys_setreuid(a1, a2); break;
    case SYS_setregid:    ret = sys_setregid(a1, a2); break;
    case SYS_getgroups:   ret = sys_getgroups((int)a1, (uint64_t*)a2); break;
    case SYS_setgroups:   ret = sys_setgroups((int)a1, (const uint64_t*)a2); break;
    case SYS_setfsuid:    ret = sys_setfsuid(a1); break;
    case SYS_setfsgid:    ret = sys_setfsgid(a1); break;

    /* ========== Directory ========== */
    case SYS_getdents64:  ret = sys_getdents64((int)a1, (struct dirent64*)a2, (size_t)a3); break;
    case SYS_getdents:    ret = sys_getdents64((int)a1, (struct dirent64*)a2, (size_t)a3); break;
    case SYS_getcwd:      ret = sys_getcwd((char*)a1, (size_t)a2); break;
    case SYS_chdir:       ret = sys_chdir((const char*)a1); break;
    case SYS_fchdir:      ret = 0; break;
    case SYS_mkdir:       ret = sys_mkdir((const char*)a1, (int)a2); break;
    case SYS_rmdir:       ret = sys_rmdir((const char*)a1); break;
    case SYS_creat:       ret = sys_creat((const char*)a1, (int)a2); break;
    case SYS_unlink:      ret = sys_unlink((const char*)a1); break;
    case SYS_rename:      ret = 0; break;
    case SYS_link:        ret = -ENOSYS; break;
    case SYS_symlink:     ret = -ENOSYS; break;
    case SYS_readlink:    ret = -ENOSYS; break;
    case SYS_chmod:       ret = 0; break;
    case SYS_fchmod:      ret = 0; break;
    case SYS_chown:       ret = 0; break;
    case SYS_fchown:      ret = 0; break;
    case SYS_lchown:      ret = 0; break;
    case SYS_access:      ret = sys_access((const char*)a1, (int)a2); break;
    case SYS_truncate:    ret = 0; break;
    case SYS_ftruncate:   ret = 0; break;

    /* ========== Signal ========== */
    case SYS_rt_sigaction:   ret = sys_rt_sigaction((int)a1, (const struct sigaction*)a2, (struct sigaction*)a3, (size_t)a4); break;
    case SYS_rt_sigprocmask: ret = sys_rt_sigprocmask((int)a1, (const sigset_t*)a2, (sigset_t*)a3, (size_t)a4); break;
    case SYS_rt_sigreturn:   ret = sys_rt_sigreturn(); break;
    case SYS_rt_sigpending:  ret = sys_rt_sigpending((sigset_t*)a1, (size_t)a2); break;
    case SYS_rt_sigtimedwait: ret = sys_rt_sigtimedwait((const sigset_t*)a1, (void*)a2, (const struct timespec*)a3, (size_t)a4); break;
    case SYS_rt_sigsuspend:  ret = sys_rt_sigsuspend((const sigset_t*)a1, (size_t)a2); break;
    case SYS_rt_sigqueueinfo: ret = 0; break;
    case SYS_sigaltstack:    ret = sys_sigaltstack((const void*)a1, (void*)a2); break;
    case SYS_kill:           ret = sys_kill((int)a1, (int)a2); break;
    case SYS_tkill:          ret = sys_tkill((int)a1, (int)a2); break;
    case SYS_tgkill:         ret = sys_tgkill((int)a1, (int)a2, (int)a3); break;

    /* ========== Time ========== */
    case SYS_clock_gettime:  ret = sys_clock_gettime((int)a1, (struct timespec*)a2); break;
    case SYS_clock_settime:  ret = 0; break;
    case SYS_clock_getres:   ret = 0; break;
    case SYS_clock_nanosleep: ret = sys_nanosleep((const struct timespec*)a2, (struct timespec*)a3); break;
    case SYS_nanosleep:      ret = sys_nanosleep((const struct timespec*)a1, (struct timespec*)a2); break;
    case SYS_gettimeofday:   ret = sys_gettimeofday((struct timeval*)a1, (void*)a2); break;
    case SYS_time:           ret = sys_time((uint64_t*)a1); break;
    case SYS_times:          ret = 0; break;

    /* ========== I/O vector ========== */
    case SYS_readv:       ret = sys_readv((int)a1, (const struct iovec*)a2, (int)a3); break;
    case SYS_writev:      ret = sys_writev((int)a1, (const struct iovec*)a2, (int)a3); break;
    case SYS_pread64:     ret = sys_read((int)a1, a3, (size_t)a4); break;
    case SYS_pwrite64:    ret = sys_write((int)a1, a3, (size_t)a4); break;

    /* ========== FD operations ========== */
    case SYS_dup:         ret = sys_dup((int)a1); break;
    case SYS_dup2:        ret = sys_dup2((int)a1, (int)a2); break;
    case SYS_dup3:        ret = sys_dup2((int)a1, (int)a2); break;
    case SYS_fcntl:       ret = sys_fcntl((int)a1, (int)a2, a3); break;
    case SYS_ioctl:       ret = sys_ioctl((int)a1, (unsigned long)a2, a3); break;
    case SYS_fsync:       ret = 0; break;
    case SYS_fdatasync:   ret = 0; break;
    case SYS_flock:       ret = 0; break;
    case SYS_pipe:        ret = sys_pipe((int*)a1); break;
    case SYS_pipe2:       ret = sys_pipe((int*)a1); break;

    /* ========== Scheduling ========== */
    case SYS_sched_yield:           ret = sys_sched_yield(); break;
    case SYS_sched_setparam:        ret = sys_sched_setparam((long)a1, (const void*)a2); break;
    case SYS_sched_getparam:        ret = sys_sched_getparam((long)a1, (void*)a2); break;
    case SYS_sched_setscheduler:    ret = sys_sched_setscheduler((long)a1, (int)a2, (const void*)a3); break;
    case SYS_sched_getscheduler:    ret = sys_sched_getscheduler((long)a1); break;
    case SYS_sched_get_priority_max: ret = sys_sched_get_priority_max((int)a1); break;
    case SYS_sched_get_priority_min: ret = sys_sched_get_priority_min((int)a1); break;
    case SYS_sched_rr_get_interval: ret = sys_sched_rr_get_interval((long)a1, (struct timespec*)a2); break;
    case SYS_sched_setaffinity:     ret = 0; break;
    case SYS_sched_getaffinity:     ret = 0; break;

    /* ========== Poll / Select ========== */
    case SYS_poll:        ret = sys_poll((struct pollfd*)a1, (unsigned long)a2, (int)a3); break;
    case SYS_ppoll:       ret = sys_ppoll((struct pollfd*)a1, (unsigned long)a2, (const struct timespec*)a3, (const sigset_t*)a4, (size_t)a5); break;
    case SYS_select:      ret = sys_select((int)a1, (void*)a2, (void*)a3, (void*)a4, (struct timeval*)a5); break;
    case SYS_pselect6:    ret = sys_pselect6((int)a1, (void*)a2, (void*)a3, (void*)a4, (const struct timespec*)a5, (const void*)a6); break;

    /* ========== System ========== */
    case SYS_uname:       ret = sys_uname((struct utsname*)a1); break;
    case SYS_sysinfo:     ret = 0; break;
    case SYS_prctl:       ret = sys_prctl((int)a1, a2, a3, a4, a5); break;
    case SYS_arch_prctl:  ret = sys_arch_prctl((int)a1, a2); break;
    case SYS_personality: ret = sys_personality(a1); break;
    case SYS_getrlimit:   ret = sys_getrlimit((int)a1, (struct rlimit*)a2); break;
    case SYS_setrlimit:   ret = sys_setrlimit((int)a1, (const struct rlimit*)a2); break;
    case SYS_getrusage:   ret = sys_getrusage((int)a1, (struct rusage*)a2); break;
    case SYS_umask:       ret = sys_umask((int)a1); break;
    case SYS_chroot:      ret = 0; break;
    case SYS_sync:        ret = 0; break;
    case SYS_reboot:      ret = 0; break;
    case SYS_getpriority: ret = sys_getpriority((int)a1, (int)a2); break;
    case SYS_setpriority: ret = sys_setpriority((int)a1, (int)a2, (int)a3); break;
    case SYS_vhangup:     ret = 0; break;
    case SYS_modify_ldt:  ret = -ENOSYS; break;
    case SYS_ustat:       ret = 0; break;
    case SYS_statfs:      ret = 0; break;
    case SYS_fstatfs:     ret = 0; break;

    /* ========== Thread / Futex ========== */
    case SYS_set_tid_address:    ret = sys_set_tid_address(a1); break;
    case SYS_set_robust_list:    ret = sys_set_robust_list(a1, (size_t)a2); break;
    case SYS_get_robust_list:    ret = sys_get_robust_list((int)a1, (uint64_t*)a2, (size_t*)a3); break;
    case SYS_futex:              ret = sys_futex(a1, (int)a2, (uint32_t)a3, (const struct timespec*)a4, a5, (uint32_t)a6); break;
    case SYS_restart_syscall:    ret = -EINTR; break;

    /* ========== Network stubs ========== */
    case SYS_socket:       ret = sys_socket((int)a1, (int)a2, (int)a3); break;
    case SYS_connect:      ret = sys_connect((int)a1, (const struct sockaddr*)a2, a3); break;
    case SYS_bind:         ret = sys_bind((int)a1, (const struct sockaddr*)a2, a3); break;
    case SYS_listen:       ret = sys_listen((int)a1, (int)a2); break;
    case SYS_accept:       ret = sys_accept((int)a1, (struct sockaddr*)a2, (uint64_t*)a3); break;
    case SYS_setsockopt:   ret = sys_setsockopt((int)a1, (int)a2, (int)a3, (const void*)a4, a5); break;
    case SYS_getsockopt:   ret = sys_getsockopt((int)a1, (int)a2, (int)a3, (void*)a4, (uint64_t*)a5); break;
    case SYS_sendto:       ret = sys_sendto((int)a1, (const void*)a2, (size_t)a3, (int)a4, (const struct sockaddr*)a5, a6); break;
    case SYS_recvfrom:     ret = sys_recvfrom((int)a1, (void*)a2, (size_t)a3, (int)a4, (struct sockaddr*)a5, (uint64_t*)a6); break;
    case SYS_shutdown:     ret = sys_shutdown((int)a1, (int)a2); break;
    case SYS_sendmsg:      ret = sys_sendmsg((int)a1, (const void*)a2, (int)a3); break;
    case SYS_recvmsg:      ret = sys_recvmsg((int)a1, (void*)a2, (int)a3); break;
    case SYS_getsockname:  ret = sys_getsockname((int)a1, (struct sockaddr*)a2, (uint64_t*)a3); break;
    case SYS_getpeername:  ret = sys_getpeername((int)a1, (struct sockaddr*)a2, (uint64_t*)a3); break;
    case SYS_socketpair:   ret = sys_socketpair((int)a1, (int)a2, (int)a3, (int*)a4); break;
    case SYS_sendfile:     ret = sys_sendfile((int)a1, (int)a2, (int64_t*)a3, (size_t)a4); break;

    /* ========== Misc stubs ========== */
    case SYS_epoll_create:  ret = -ENOSYS; break;
    case SYS_epoll_ctl:     ret = -ENOSYS; break;
    case SYS_epoll_wait:    ret = -ENOSYS; break;
    case SYS_epoll_pwait:   ret = -ENOSYS; break;
    case SYS_eventfd:       ret = -ENOSYS; break;
    case SYS_signalfd:      ret = -ENOSYS; break;
    case SYS_timerfd_create: ret = -ENOSYS; break;
    case SYS_inotify_init:  ret = -ENOSYS; break;
    case SYS_io_setup:      ret = -ENOSYS; break;
    case SYS_io_destroy:    ret = -ENOSYS; break;
    case SYS_io_submit:     ret = -ENOSYS; break;
    case SYS_io_cancel:     ret = -ENOSYS; break;
    case SYS_io_getevents:  ret = -ENOSYS; break;
    case SYS_membarrier:    ret = 0; break;
    case SYS_getrandom:     ret = -ENOSYS; break;
    case SYS_seccomp:       ret = 0; break;
    case SYS_pidfd_send_signal: ret = -ENOSYS; break;
    case SYS_faccessat:     ret = sys_access((const char*)a2, (int)a3); break;
    case SYS_openat:        ret = sys_open((const char*)a2, (int)a3, (int)a4); break;
    case SYS_mkdirat:       ret = sys_mkdir((const char*)a2, (int)a3); break;
    case SYS_unlinkat:      ret = sys_unlink((const char*)a2); break;
    case SYS_renameat:      ret = 0; break;
    case SYS_utimensat:     ret = 0; break;
    case SYS_utimes:        ret = 0; break;
    case SYS_fallocate:     ret = 0; break;
    case SYS_prlimit64:     ret = sys_getrlimit((int)a2, (struct rlimit*)a3); break;
    case SYS_getcpu:        ret = 0; break;
    case SYS_splice:        ret = -ENOSYS; break;
    case SYS_tee:           ret = -ENOSYS; break;
    case SYS_vmsplice:      ret = -ENOSYS; break;
    case SYS_sync_file_range: ret = 0; break;
    case SYS_swapoff:       ret = 0; break;
    case SYS_swapon:        ret = 0; break;
    case SYS_mount:         ret = 0; break;
    case SYS_umount2:       ret = 0; break;
    case SYS_semget:        ret = -ENOSYS; break;
    case SYS_semop:         ret = -ENOSYS; break;
    case SYS_semctl:        ret = -ENOSYS; break;
    case SYS_unshare:       ret = -ENOSYS; break;
    /* ========== Default ========== */
    default:
        serial_puts("syscall: unknown nr=");
        serial_putdec64(nr);
        serial_puts("\n");
        ret = -ENOSYS;
        break;
    }

    tf->rax = (uint64_t)ret;
}