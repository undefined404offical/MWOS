#ifndef SYSCALL_NR_H
#define SYSCALL_NR_H

/* Linux-compatible syscall numbers for x86_64 */
#define SYS_read                0
#define SYS_write               1
#define SYS_open                2
#define SYS_close               3
#define SYS_stat                4
#define SYS_fstat               5
#define SYS_lstat               6
#define SYS_poll                7
#define SYS_lseek               8
#define SYS_mmap                9
#define SYS_mprotect            10
#define SYS_munmap              11
#define SYS_brk                 12
#define SYS_rt_sigaction        13
#define SYS_rt_sigprocmask      14
#define SYS_rt_sigreturn        15
#define SYS_ioctl               16
#define SYS_pread64             17
#define SYS_pwrite64            18
#define SYS_readv               19
#define SYS_writev              20
#define SYS_access              21
#define SYS_pipe                22
#define SYS_select              23
#define SYS_sched_yield         24
#define SYS_mremap              25
#define SYS_msync               26
#define SYS_mincore             27
#define SYS_madvise             28
#define SYS_dup                 32
#define SYS_dup2                33
#define SYS_nanosleep           35
#define SYS_getpid              39
#define SYS_sendfile            40
#define SYS_socket              41
#define SYS_connect             42
#define SYS_accept              43
#define SYS_sendto              44
#define SYS_recvfrom            45
#define SYS_sendmsg             46
#define SYS_recvmsg             47
#define SYS_shutdown            48
#define SYS_bind                49
#define SYS_listen              50
#define SYS_getsockname         51
#define SYS_getpeername         52
#define SYS_socketpair          53
#define SYS_setsockopt          54
#define SYS_getsockopt          55
#define SYS_clone               56
#define SYS_fork                57
#define SYS_vfork               58
#define SYS_execve              59
#define SYS_exit                60
#define SYS_wait4               61
#define SYS_kill                62
#define SYS_uname               63
#define SYS_semget              64
#define SYS_semop               65
#define SYS_semctl              66
#define SYS_fcntl               72
#define SYS_flock               73
#define SYS_fsync               74
#define SYS_fdatasync           75
#define SYS_truncate            76
#define SYS_ftruncate           77
#define SYS_getdents            78
#define SYS_getcwd              79
#define SYS_chdir               80
#define SYS_fchdir              81
#define SYS_rename              82
#define SYS_mkdir               83
#define SYS_rmdir               84
#define SYS_creat               85
#define SYS_link                86
#define SYS_unlink              87
#define SYS_symlink             88
#define SYS_readlink            89
#define SYS_chmod               90
#define SYS_fchmod              91
#define SYS_chown               92
#define SYS_fchown              93
#define SYS_lchown              94
#define SYS_umask               95
#define SYS_gettimeofday        96
#define SYS_getrlimit           97
#define SYS_getrusage           98
#define SYS_getuid              102
#define SYS_getgid              104
#define SYS_geteuid             107
#define SYS_getegid             108
#define SYS_setpgid             109
#define SYS_getppid             110
#define SYS_getpgrp             111
#define SYS_setsid              112
#define SYS_setreuid            113
#define SYS_setregid            114
#define SYS_getgroups           115
#define SYS_setgroups           116
#define SYS_setresuid           117
#define SYS_getresuid           118
#define SYS_setresgid           119
#define SYS_getresgid           120
#define SYS_getpgid             121
#define SYS_setfsuid            122
#define SYS_setfsgid            123
#define SYS_rt_sigpending       127
#define SYS_rt_sigtimedwait     128
#define SYS_rt_sigqueueinfo     129
#define SYS_rt_sigsuspend       130
#define SYS_sigaltstack         131
#define SYS_personality         135
#define SYS_ustat               136
#define SYS_statfs              137
#define SYS_fstatfs             138
#define SYS_getpriority         140
#define SYS_setpriority         141
#define SYS_sched_setparam      142
#define SYS_sched_getparam      143
#define SYS_sched_setscheduler  144
#define SYS_sched_getscheduler  145
#define SYS_sched_get_priority_max 146
#define SYS_sched_get_priority_min 147
#define SYS_sched_rr_get_interval  148
#define SYS_mlock               149
#define SYS_munlock             150
#define SYS_mlockall            151
#define SYS_munlockall          152
#define SYS_prctl               157
#define SYS_arch_prctl          158
#define SYS_setrlimit           160
#define SYS_chroot              161
#define SYS_sync                162
#define SYS_mount               165
#define SYS_umount2             166
#define SYS_gettid              186
#define SYS_tkill               200
#define SYS_time                201
#define SYS_futex               202
#define SYS_getsid              124
#define SYS_sched_setaffinity   203
#define SYS_sched_getaffinity   204
#define SYS_io_setup            206
#define SYS_io_destroy          207
#define SYS_io_getevents        208
#define SYS_io_submit           209
#define SYS_io_cancel           210
#define SYS_epoll_create        213
#define SYS_getdents64          217
#define SYS_set_tid_address     218
#define SYS_clock_settime       227
#define SYS_clock_gettime       228
#define SYS_clock_getres        229
#define SYS_clock_nanosleep     230
#define SYS_exit_group          231
#define SYS_epoll_wait          232
#define SYS_epoll_ctl           233
#define SYS_tgkill              234
#define SYS_utimes              235
#define SYS_mbind               237
#define SYS_set_mempolicy       238
#define SYS_get_mempolicy       239
#define SYS_openat              257
#define SYS_mkdirat             258
#define SYS_newfstatat          262
#define SYS_unlinkat            263
#define SYS_renameat            264
#define SYS_faccessat           269
#define SYS_pselect6            270
#define SYS_ppoll               271
#define SYS_unshare             272
#define SYS_set_robust_list     273
#define SYS_get_robust_list     274
#define SYS_splice              275
#define SYS_sync_file_range     277
#define SYS_utimensat           280
#define SYS_epoll_pwait         281
#define SYS_fallocate           285
#define SYS_dup3                292
#define SYS_pipe2               293
#define SYS_prlimit64           302
#define SYS_getrandom           318
#define SYS_memfd_create        319
#define SYS_execveat            322
#define SYS_copy_file_range     326
#define SYS_preadv2             327
#define SYS_pwritev2            328
#define SYS_statx               332

/* Additional syscalls used by kernel */
#define SYS_restart_syscall     219
#define SYS_inotify_init        253
#define SYS_eventfd             284
#define SYS_signalfd            289
#define SYS_timerfd_create      283
#define SYS_vmsplice            296
#define SYS_tee                 297
#define SYS_pidfd_send_signal   299
#define SYS_getcpu              309
#define SYS_seccomp             317
#define SYS_membarrier          324
#define SYS_vhangup             153
#define SYS_modify_ldt          154
#define SYS_swapon              167
#define SYS_swapoff             168
#define SYS_reboot              169
#define SYS_sysinfo             99
#define SYS_times               100
#define SYS_init_module         175
#define SYS_kexec_load          246
#define SYS_lookup_dcookie      212
#define SYS_migrate_pages       256
#define SYS_move_pages          279
#define SYS_add_key             248
#define SYS_request_key         249
#define SYS_keyctl              250
#define SYS_kcmp                311
#define SYS_finit_module        313

/* MWOS internal errno values (negative) */
#define EPERM    1
#define ENOENT   2
#define ESRCH    3
#define EINTR    4
#define EIO      5
#define ENXIO    6
#define E2BIG    7
#define ENOEXEC  8
#define EBADF    9
#define ECHILD   10
#define EAGAIN   11
#define ENOMEM   12
#define EACCES   13
#define EFAULT   14
#define ENOTBLK  15
#define EBUSY    16
#define EEXIST   17
#define EXDEV    18
#define ENODEV   19
#define ENOTDIR  20
#define EISDIR   21
#define EINVAL   22
#define ENFILE   23
#define EMFILE   24
#define ENOTTY   25
#define ETXTBSY  26
#define EFBIG    27
#define ENOSPC   28
#define ESPIPE   29
#define EROFS    30
#define EMLINK   31
#define EPIPE    32
#define EDOM     33
#define ERANGE   34
#define ENOSYS   38
#define ENOTSUP  95
#define EAFNOSUPPORT 97
#define EOPNOTSUPP 95

/* fcntl.h */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x40
#define O_EXCL      0x80
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_NONBLOCK  0x800

/* mmap flags */
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_ANONYMOUS   0x20
#define MAP_FAILED      ((void*)-1)

/* lseek whence */
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

/* arch_prctl codes */
#define ARCH_SET_FS     0x1002
#define ARCH_GET_FS     0x1003

/* poll */
#define POLLIN      0x001
#define POLLOUT     0x004
#define POLLERR     0x008
#define POLLHUP     0x010
#define POLLNVAL    0x020
#define POLLRDNORM  0x040
#define POLLWRNORM  0x100

/* ioctl */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TIOCGWINSZ  0x5413
#define TIOCGPGRP   0x540F
#define FIONREAD    0x541B

/* fcntl cmds */
#define F_DUPFD     0
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4
#define F_GETLK     5
#define F_SETLK     6
#define F_SETLKW    7
#define F_DUPFD_CLOEXEC 1030

/* uio */
#define UIO_MAXIOV  1024

/* clock types */
#define CLOCK_REALTIME          0
#define CLOCK_MONOTONIC         1
#define CLOCK_PROCESS_CPUTIME_ID  2
#define CLOCK_THREAD_CPUTIME_ID   3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7

/* wait4 */
#define WNOHANG     1
#define WUNTRACED   2
#define WEXITED     4
#define WCONTINUED  8
#define WNOWAIT     0x1000000

/* sigaction */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#define SA_RESTORER 0x04000000
#define SA_SIGINFO  0x00000004
#define SA_ONSTACK  0x08000000
#define SA_RESTART  0x10000000
#define SA_NODEFER  0x40000000
#define SA_RESETHAND 0x80000000

/* utsname */
#define UTSNAME_MAX_LEN 65

/* rlimit */
#define RLIMIT_NOFILE  7
#define RLIMIT_STACK   3
#define RLIMIT_AS      9

/* iovec */
struct iovec {
    void  *iov_base;
    size_t iov_len;
};

/* stat structure (x86_64) */
struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime;
    uint64_t st_atime_nsec;
    uint64_t st_mtime;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime;
    uint64_t st_ctime_nsec;
    int64_t  __unused[3];
};

/* sockaddr for socket stubs */
struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
};

/* timespec */
struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* timeval */
struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

/* sigset_t */
typedef struct {
    unsigned long __bits[128/sizeof(unsigned long)];
} sigset_t;

/* sigaction */
struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, void*, void*);
    };
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    sigset_t sa_mask;
};

/* pollfd */
struct pollfd {
    int fd;
    short events;
    short revents;
};

/* dirent */
struct dirent64 {
    uint64_t  d_ino;
    int64_t   d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

/* utsname */
struct utsname {
    char sysname[UTSNAME_MAX_LEN];
    char nodename[UTSNAME_MAX_LEN];
    char release[UTSNAME_MAX_LEN];
    char version[UTSNAME_MAX_LEN];
    char machine[UTSNAME_MAX_LEN];
    char domainname[UTSNAME_MAX_LEN];
};

/* rlimit */
struct rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

#endif /* SYSCALL_NR_H */