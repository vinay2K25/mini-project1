#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/user.h>
#include "snoop.h"
extern char **environ;

typedef struct SyscallEntry {
    long syscall_number;
    unsigned long long call_count;
    long double total_time;
    size_t first_seen;
} SyscallEntry;

typedef struct SyscallName {
    long number;
    const char *name;
} SyscallName;

static const SyscallName syscall_names[] = {
    {0, "read"},
    {1, "write"},
    {2, "open"},
    {3, "close"},
    {4, "stat"},
    {5, "fstat"},
    {6, "lstat"},
    {7, "poll"},
    {8, "lseek"},
    {9, "mmap"},
    {10, "mprotect"},
    {11, "munmap"},
    {12, "brk"},
    {13, "rt_sigaction"},
    {14, "rt_sigprocmask"},
    {15, "rt_sigreturn"},
    {16, "ioctl"},
    {17, "pread64"},
    {18, "pwrite64"},
    {19, "readv"},
    {20, "writev"},
    {21, "access"},
    {22, "pipe"},
    {23, "select"},
    {24, "sched_yield"},
    {25, "mremap"},
    {26, "msync"},
    {27, "mincore"},
    {28, "madvise"},
    {29, "shmget"},
    {30, "shmat"},
    {31, "shmctl"},
    {32, "dup"},
    {33, "dup2"},
    {34, "pause"},
    {35, "nanosleep"},
    {36, "getitimer"},
    {37, "alarm"},
    {38, "setitimer"},
    {39, "getpid"},
    {40, "sendfile"},
    {41, "socket"},
    {42, "connect"},
    {43, "accept"},
    {44, "sendto"},
    {45, "recvfrom"},
    {46, "sendmsg"},
    {47, "recvmsg"},
    {48, "shutdown"},
    {49, "bind"},
    {50, "listen"},
    {51, "getsockname"},
    {52, "getpeername"},
    {53, "socketpair"},
    {54, "setsockopt"},
    {55, "getsockopt"},
    {56, "clone"},
    {57, "fork"},
    {58, "vfork"},
    {59, "execve"},
    {60, "exit"},
    {61, "wait4"},
    {62, "kill"},
    {63, "uname"},
    {64, "semget"},
    {65, "semop"},
    {66, "semctl"},
    {67, "shmdt"},
    {68, "msgget"},
    {69, "msgsnd"},
    {70, "msgrcv"},
    {71, "msgctl"},
    {72, "fcntl"},
    {73, "flock"},
    {74, "fsync"},
    {75, "fdatasync"},
    {76, "truncate"},
    {77, "ftruncate"},
    {78, "getdents"},
    {79, "getcwd"},
    {80, "chdir"},
    {81, "fchdir"},
    {82, "rename"},
    {83, "mkdir"},
    {84, "rmdir"},
    {85, "creat"},
    {86, "link"},
    {87, "unlink"},
    {88, "symlink"},
    {89, "readlink"},
    {90, "chmod"},
    {91, "fchmod"},
    {92, "chown"},
    {93, "fchown"},
    {94, "lchown"},
    {95, "umask"},
    {96, "gettimeofday"},
    {97, "getrlimit"},
    {98, "getrusage"},
    {99, "sysinfo"},
    {100, "times"},
    {101, "ptrace"},
    {102, "getuid"},
    {103, "syslog"},
    {104, "getgid"},
    {105, "setuid"},
    {106, "setgid"},
    {107, "geteuid"},
    {108, "getegid"},
    {109, "setpgid"},
    {110, "getppid"},
    {111, "getpgrp"},
    {112, "setsid"},
    {113, "setreuid"},
    {114, "setregid"},
    {115, "getgroups"},
    {116, "setgroups"},
    {117, "setresuid"},
    {118, "getresuid"},
    {119, "setresgid"},
    {120, "getresgid"},
    {121, "getpgid"},
    {122, "setfsuid"},
    {123, "setfsgid"},
    {124, "getsid"},
    {125, "capget"},
    {126, "capset"},
    {127, "rt_sigpending"},
    {128, "rt_sigtimedwait"},
    {129, "rt_sigqueueinfo"},
    {130, "rt_sigsuspend"},
    {131, "sigaltstack"},
    {132, "utime"},
    {133, "mknod"},
    {134, "uselib"},
    {135, "personality"},
    {136, "ustat"},
    {137, "statfs"},
    {138, "fstatfs"},
    {139, "sysfs"},
    {140, "getpriority"},
    {141, "setpriority"},
    {142, "sched_setparam"},
    {143, "sched_getparam"},
    {144, "sched_setscheduler"},
    {145, "sched_getscheduler"},
    {146, "sched_get_priority_max"},
    {147, "sched_get_priority_min"},
    {148, "sched_rr_get_interval"},
    {149, "mlock"},
    {150, "munlock"},
    {151, "mlockall"},
    {152, "munlockall"},
    {153, "vhangup"},
    {154, "modify_ldt"},
    {155, "pivot_root"},
    {156, "_sysctl"},
    {157, "prctl"},
    {158, "arch_prctl"},
    {159, "adjtimex"},
    {160, "setrlimit"},
    {161, "chroot"},
    {162, "sync"},
    {163, "acct"},
    {164, "settimeofday"},
    {165, "mount"},
    {166, "umount2"},
    {167, "swapon"},
    {168, "swapoff"},
    {169, "reboot"},
    {170, "sethostname"},
    {171, "setdomainname"},
    {172, "iopl"},
    {173, "ioperm"},
    {174, "create_module"},
    {175, "init_module"},
    {176, "delete_module"},
    {177, "get_kernel_syms"},
    {178, "query_module"},
    {179, "quotactl"},
    {180, "nfsservctl"},
    {181, "getpmsg"},
    {182, "putpmsg"},
    {183, "afs_syscall"},
    {184, "tuxcall"},
    {185, "security"},
    {186, "gettid"},
    {187, "readahead"},
    {188, "setxattr"},
    {189, "lsetxattr"},
    {190, "fsetxattr"},
    {191, "getxattr"},
    {192, "lgetxattr"},
    {193, "fgetxattr"},
    {194, "listxattr"},
    {195, "llistxattr"},
    {196, "flistxattr"},
    {197, "removexattr"},
    {198, "lremovexattr"},
    {199, "fremovexattr"},
    {200, "tkill"},
    {201, "time"},
    {202, "futex"},
    {203, "sched_setaffinity"},
    {204, "sched_getaffinity"},
    {205, "set_thread_area"},
    {206, "io_setup"},
    {207, "io_destroy"},
    {208, "io_getevents"},
    {209, "io_submit"},
    {210, "io_cancel"},
    {211, "get_thread_area"},
    {212, "lookup_dcookie"},
    {213, "epoll_create"},
    {214, "epoll_ctl_old"},
    {215, "epoll_wait_old"},
    {216, "remap_file_pages"},
    {217, "getdents64"},
    {218, "set_tid_address"},
    {219, "restart_syscall"},
    {220, "semtimedop"},
    {221, "fadvise64"},
    {222, "timer_create"},
    {223, "timer_settime"},
    {224, "timer_gettime"},
    {225, "timer_getoverrun"},
    {226, "timer_delete"},
    {227, "clock_settime"},
    {228, "clock_gettime"},
    {229, "clock_getres"},
    {230, "clock_nanosleep"},
    {231, "exit_group"},
    {232, "epoll_wait"},
    {233, "epoll_ctl"},
    {234, "tgkill"},
    {235, "utimes"},
    {236, "vserver"},
    {237, "mbind"},
    {238, "set_mempolicy"},
    {239, "get_mempolicy"},
    {240, "mq_open"},
    {241, "mq_unlink"},
    {242, "mq_timedsend"},
    {243, "mq_timedreceive"},
    {244, "mq_notify"},
    {245, "mq_getsetattr"},
    {246, "kexec_load"},
    {247, "waitid"},
    {248, "add_key"},
    {249, "request_key"},
    {250, "keyctl"},
    {251, "ioprio_set"},
    {252, "ioprio_get"},
    {253, "inotify_init"},
    {254, "inotify_add_watch"},
    {255, "inotify_rm_watch"},
    {256, "migrate_pages"},
    {257, "openat"},
    {258, "mkdirat"},
    {259, "mknodat"},
    {260, "fchownat"},
    {261, "futimesat"},
    {262, "newfstatat"},
    {263, "unlinkat"},
    {264, "renameat"},
    {265, "linkat"},
    {266, "symlinkat"},
    {267, "readlinkat"},
    {268, "fchmodat"},
    {269, "faccessat"},
    {270, "pselect6"},
    {271, "ppoll"},
    {272, "unshare"},
    {273, "set_robust_list"},
    {274, "get_robust_list"},
    {275, "splice"},
    {276, "tee"},
    {277, "sync_file_range"},
    {278, "vmsplice"},
    {279, "move_pages"},
    {280, "utimensat"},
    {281, "epoll_pwait"},
    {282, "signalfd"},
    {283, "timerfd_create"},
    {284, "eventfd"},
    {285, "fallocate"},
    {286, "timerfd_settime"},
    {287, "timerfd_gettime"},
    {288, "accept4"},
    {289, "signalfd4"},
    {290, "eventfd2"},
    {291, "epoll_create1"},
    {292, "dup3"},
    {293, "pipe2"},
    {294, "inotify_init1"},
    {295, "preadv"},
    {296, "pwritev"},
    {297, "rt_tgsigqueueinfo"},
    {298, "perf_event_open"},
    {299, "recvmmsg"},
    {300, "fanotify_init"},
    {301, "fanotify_mark"},
    {302, "prlimit64"},
    {303, "name_to_handle_at"},
    {304, "open_by_handle_at"},
    {305, "clock_adjtime"},
    {306, "syncfs"},
    {307, "sendmmsg"},
    {308, "setns"},
    {309, "getcpu"},
    {310, "process_vm_readv"},
    {311, "process_vm_writev"},
    {312, "kcmp"},
    {313, "finit_module"},
    {314, "sched_setattr"},
    {315, "sched_getattr"},
    {316, "renameat2"},
    {317, "seccomp"},
    {318, "getrandom"},
    {319, "memfd_create"},
    {320, "kexec_file_load"},
    {321, "bpf"},
    {322, "execveat"},
    {323, "userfaultfd"},
    {324, "userfaultfd"},
    {325, "membarrier"},
    {326, "mlock2"},
    {327, "copy_file_range"},
    {328, "preadv2"},
    {329, "pwritev2"},
    {330, "pkey_mprotect"},
    {331, "pkey_alloc"},
    {332, "pkey_free"},
    {333, "statx"},
    {334, "io_pgetevents"},
    {424, "pidfd_send_signal"},
    {425, "io_uring_setup"},
    {426, "io_uring_enter"},
    {427, "io_uring_register"},
    {428, "open_tree"},
    {429, "move_mount"},
    {430, "fsopen"},
    {431, "fsconfig"},
    {432, "fsmount"},
    {433, "fspick"},
    {434, "pidfd_open"},
    {435, "clone3"},
    {436, "close_range"},
    {437, "openat2"},
    {438, "pidfd_getfd"},
    {439, "faccessat2"},
    {440, "process_madvise"},
    {441, "epoll_pwait2"},
    {442, "mount_setattr"},
    {443, "quotactl_fd"},
    {444, "landlock_create_ruleset"},
    {445, "landlock_add_rule"},
    {446, "landlock_restrict_self"},
    {447, "memfd_secret"},
    {448, "process_mrelease"},
    {449, "futex_waitv"},
    {450, "set_mempolicy_home_node"}};

static const size_t syscall_name_count = sizeof(syscall_names) / sizeof(syscall_names[0]);

static const char *get_syscall_name(long number, char *unknown_name, size_t size) {
    for (size_t i = 0; i < syscall_name_count; i++) {
        if (syscall_names[i].number == number) {
            return syscall_names[i].name;
        }
    }
    snprintf(unknown_name, size, "syscall_%ld", number);
    return unknown_name;
}

static bool is_executable_file(const char *path) {
    struct stat information;
    if (stat(path, &information) == -1) {
        return false;
    }

    if (!S_ISREG(information.st_mode)) {
        return false;
    }
    return access(path, X_OK) == 0;
}

static bool resolve_snoop_command(const char *command, char *resolved_path, size_t size) {
    if (command == NULL || command[0] == '\0') {
        return false;
    }
    if (strchr(command, '/') != NULL) {
        if (!is_executable_file(command)) {
            return false;
        }
        int written = snprintf(resolved_path, size, "%s", command);
        return written >= 0 && (size_t)written < size;
    }
    char current_directory[PATH_MAX];
    if (getcwd(current_directory, sizeof(current_directory)) != NULL) {
        char candidate[PATH_MAX];
        int written = snprintf(candidate, sizeof(candidate), "%s/%s", current_directory, command);
        if (written >= 0 && (size_t)written < sizeof(candidate) && is_executable_file(candidate)) {
            written = snprintf(resolved_path, size, "%s", candidate);
            return written >= 0 && (size_t)written < size;
        }
    }
    const char *path = getenv("PATH");
    if (path == NULL) {
        return false;
    }
    char *path_copy = malloc(strlen(path) + 1);
    if (path_copy == NULL) {
        return false;
    }
    strcpy(path_copy, path);
    char *directory = path_copy;
    while (true) {
        char *separator = strchr(directory, ':');
        if (separator != NULL) {
            *separator = '\0';
        }
        char candidate[PATH_MAX];
        if (directory[0] == '\0') {
            if (getcwd(current_directory, sizeof(current_directory)) == NULL) {
                free(path_copy);
                return false;
            }
            int written = snprintf(candidate, sizeof(candidate), "%s/%s", current_directory, command);
            if (written < 0 || (size_t)written >= sizeof(candidate)) {
                free(path_copy);
                return false;
            }
        }
        else {
            int written = snprintf(candidate, sizeof(candidate), "%s/%s", directory, command);
            if (written < 0 || (size_t)written >= sizeof(candidate)) {
                free(path_copy);
                return false;
            }
        }
        if (is_executable_file(candidate)) {
            int written = snprintf(resolved_path, size, "%s", candidate);
            free(path_copy);
            return written >= 0 && (size_t)written < size;
        }
        if (separator == NULL) {
            break;
        }
        directory = separator + 1;
    }
    free(path_copy);
    return false;
}

static size_t count_arguments(Token *tokens) {
    size_t count = 0;
    Token *current = tokens;
    while (current != NULL) {
        if (current->type != TOKEN_WORD) {
            break;
        }
        count++;
        current = current->next;
    }
    return count;
}

static char **build_snoop_argv(Token *tokens) {
    size_t argument_count = count_arguments(tokens);
    char **argv = malloc((argument_count + 1) * sizeof(char *));
    if (argv == NULL) {
        return NULL;
    }
    Token *current = tokens;
    for (size_t i = 0; i < argument_count; i++) {
        argv[i] = current->value;
        current = current->next;
    }
    argv[argument_count] = NULL;
    return argv;
}

static long double timespec_to_seconds(const struct timespec *time) {
    return (long double)time->tv_sec + (long double)time->tv_nsec / 1000000000.0L;
}

static long double elapsed_time(const struct timespec *start, const struct timespec *end) {
    return timespec_to_seconds(end) - timespec_to_seconds(start);
}

static SyscallEntry *find_syscall(SyscallEntry *entries, size_t count, long syscall_number) {
    for (size_t i = 0; i < count; i++) {
        if (entries[i].syscall_number == syscall_number) {
            return &entries[i];
        }
    }
    return NULL;
}

static bool record_syscall(SyscallEntry **entries, size_t *count, size_t *capacity, long syscall_number, long double duration) {
    SyscallEntry *entry = find_syscall(*entries, *count, syscall_number);
    if (entry != NULL) {
        entry->call_count++;
        entry->total_time += duration;
        return true;
    }
    if (*count == *capacity) {
        size_t new_capacity = (*capacity == 0) ? 32 : (*capacity * 2);
        SyscallEntry *new_entries = realloc(*entries, new_capacity * sizeof(SyscallEntry));
        if (new_entries == NULL) {
            return false;
        }
        *entries = new_entries;
        *capacity = new_capacity;
    }
    entry = &(*entries)[*count];
    entry->syscall_number = syscall_number;
    entry->call_count = 1;
    entry->total_time = duration;
    entry->first_seen = *count;
    (*count)++;
    return true;
}

static int compare_syscalls(const void *first, const void *second) {
    const SyscallEntry *a = first;
    const SyscallEntry *b = second;
    if (a->call_count > b->call_count) {
        return -1;
    }
    if (a->call_count < b->call_count) {
        return 1;
    }
    if (a->first_seen < b->first_seen) {
        return -1;
    }
    if (a->first_seen > b->first_seen) {
        return 1;
    }
    return 0;
}

static void print_summary(SyscallEntry *entries, size_t count) {
    qsort(entries, count, sizeof(SyscallEntry), compare_syscalls);
    printf("%-24s %-10s %s\n", "syscall", "calls", "time");
    for (size_t i = 0; i < count; i++) {
        char unknown_name[64];
        const char *name = get_syscall_name(entries[i].syscall_number, unknown_name, sizeof(unknown_name));
        printf("%-24s %-10llu %.3Lfs\n", name, entries[i].call_count, entries[i].total_time);
    }
}

static bool continue_tracee(pid_t pid, int signal_number, int *status) {
    if (ptrace(PTRACE_SYSCALL, pid, NULL, (void *)(long)signal_number) == -1) {
        return false;
    }
    while (waitpid(pid, status, 0) == -1) {
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

static bool snoop_command_mode(Token *tokens) {
    char resolved_path[PATH_MAX];
    if (!resolve_snoop_command(tokens->next->value, resolved_path, sizeof(resolved_path))) {
        printf("snoop: command not found\n");
        return true;
    }
    char **argv = build_snoop_argv(tokens->next);
    if (argv == NULL) {
        perror("malloc");
        return false;
    }
    pid_t child = fork();
    if (child == -1) {
        perror("fork");
        free(argv);
        return false;
    }
    if (child == 0) {
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) == -1) {
            _exit(EXIT_FAILURE);
        }
        if (raise(SIGSTOP) != 0) {
            _exit(EXIT_FAILURE);
        }
        execve(resolved_path, argv, environ);
        _exit(EXIT_FAILURE);
    }
    free(argv);
    int status;
    while (waitpid(child, &status, 0) == -1) {
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    if (!WIFSTOPPED(status)) {
        return false;
    }
    if (ptrace(PTRACE_SETOPTIONS, child, NULL, PTRACE_O_TRACESYSGOOD) == -1) {
        perror("ptrace");
        return false;
    }

    if (ptrace(PTRACE_SYSCALL, child, NULL, NULL) == -1) {
        perror("ptrace");
        return false;
    }
    while (waitpid(child, &status, 0) == -1) {
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
        printf("%-24s %-10s %s\n", "syscall", "calls", "time");
        return true;
    }
    if (WIFSTOPPED(status)) {
        SyscallEntry *entries = NULL;
        size_t entry_count = 0;
        size_t entry_capacity = 0;
        bool at_entry = true;
        long current_syscall = -1;
        struct timespec syscall_start;
        while (true) {
            int stop_signal = WSTOPSIG(status);
            if (stop_signal == (SIGTRAP | 0x80)) {
                struct user_regs_struct registers;
                if (ptrace(PTRACE_GETREGS, child, NULL, &registers) == -1) {
                    free(entries);
                    return false;
                }
                if (at_entry) {
                    current_syscall = (long)registers.orig_rax;
                    if (clock_gettime(CLOCK_MONOTONIC, &syscall_start) == -1) {
                        free(entries);
                        return false;
                    }
                    at_entry = false;
                }
                else {
                    struct timespec syscall_end;
                    if (clock_gettime(CLOCK_MONOTONIC, &syscall_end) == -1) {
                        free(entries);
                        return false;
                    }
                    long double duration = elapsed_time(&syscall_start, &syscall_end);
                    if (!record_syscall(&entries, &entry_count, &entry_capacity, current_syscall, duration)) {
                        free(entries);
                        return false;
                    }
                    at_entry = true;
                }
                if (!continue_tracee(child, 0, &status)) {
                    free(entries);
                    return false;
                }
            }
            else {
                int signal_to_deliver = (stop_signal == SIGTRAP) ? 0 : stop_signal;
                if (!continue_tracee(child, signal_to_deliver, &status)) {
                    free(entries);
                    return false;
                }
            }
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                break;
            }
        }
        print_summary(entries, entry_count);
        free(entries);
    }
    return true;
}

static bool snoop_pid_mode(pid_t pid) {
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) == -1) {
        printf("snoop: no such process\n");
        return true;
    }
    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno == EINTR) {
            continue;
        }
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return false;
    }
    if (!WIFSTOPPED(status)) {
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return false;
    }
    if (ptrace(PTRACE_SETOPTIONS, pid, NULL, PTRACE_O_TRACESYSGOOD) == -1) {
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return false;
    }
    if (ptrace(PTRACE_SYSCALL, pid, NULL, NULL) == -1) {
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return false;
    }
    SyscallEntry *entries = NULL;
    size_t entry_count = 0;
    size_t entry_capacity = 0;
    bool at_entry = true;
    long current_syscall = -1;
    struct timespec syscall_start;
    while (true) {
        while (waitpid(pid, &status, 0) == -1) {
            if (errno == EINTR) {
                continue;
            }
            free(entries);
            return false;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            break;
        }
        if (!WIFSTOPPED(status)) {
            continue;
        }
        int stop_signal = WSTOPSIG(status);
        if (stop_signal == (SIGTRAP | 0x80)) {
            struct user_regs_struct registers;
            if (ptrace(PTRACE_GETREGS, pid, NULL, &registers) == -1) {
                free(entries);
                return false;
            }
            if (at_entry) {
                current_syscall = (long)registers.orig_rax;
                if (clock_gettime(CLOCK_MONOTONIC, &syscall_start) == -1) {
                    free(entries);
                    return false;
                }
                at_entry = false;
            }
            else {
                struct timespec syscall_end;
                if (clock_gettime(CLOCK_MONOTONIC, &syscall_end) == -1) {
                    free(entries);
                    return false;
                }
                long double duration = elapsed_time(&syscall_start, &syscall_end);
                if (!record_syscall(&entries, &entry_count, &entry_capacity, current_syscall, duration)) {
                    free(entries);
                    return false;
                }
                at_entry = true;
            }
            if (ptrace(PTRACE_SYSCALL, pid, NULL, NULL) == -1) {
                free(entries);
                return false;
            }
        }
        else {
            int signal_to_deliver = (stop_signal == SIGTRAP) ? 0 : stop_signal;
            if (ptrace(PTRACE_SYSCALL, pid, NULL, (void *)(long)signal_to_deliver) == -1) {
                free(entries);
                return false;
            }
        }
    }
    print_summary(entries, entry_count);
    free(entries);
    return true;
}

static bool parse_pid(const char *text, pid_t *pid) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (!isdigit((unsigned char)text[i])) {
            return false;
        }
    }
    errno = 0;
    char *end;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno == ERANGE || *end != '\0' || value == 0 || value > INT_MAX) {
        return false;
    }
    *pid = (pid_t)value;
    return true;
}

bool execute_snoop(Token *tokens) {
    if (tokens == NULL || tokens->type != TOKEN_WORD || strcmp(tokens->value, "snoop") != 0) {
        return false;
    }
    Token *current = tokens->next;
    if (current == NULL) {
        printf("snoop: invalid syntax\n");
        return true;
    }
    if (strcmp(current->value, "-p") == 0) {
        current = current->next;
        if (current == NULL || current->type != TOKEN_WORD || current->next != NULL) {
            printf("snoop: invalid syntax\n");
            return true;
        }
        pid_t pid;
        if (!parse_pid(current->value, &pid)) {
            printf("snoop: no such process\n");
            return true;
        }
        return snoop_pid_mode(pid);
    }
    current = tokens->next;
    while (current != NULL) {
        if (current->type != TOKEN_WORD) {
            printf("snoop: invalid syntax\n");
            return true;
        }
        current = current->next;
    }
    return snoop_command_mode(tokens);
}