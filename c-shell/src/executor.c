#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include "executor.h"
#include "builtins.h"

#define MAX_BACKGROUND_PROCESSES 1024
static void build_command_string(Token *tokens, char *buffer, size_t buffer_size);

typedef enum {
    PROCESS_RUNNING,
    PROCESS_STOPPED
} ProcessState;

typedef struct {
    pid_t pid;
    pid_t pgid;
    pid_t *pids;
    ProcessState *states;
    char **commands;
    size_t process_count;
    unsigned long job_number;
    char command[4096];
    bool active;
    bool completed;
    bool normal;
} BackgroundJob;
static BackgroundJob background_jobs[MAX_BACKGROUND_PROCESSES];
static unsigned long next_job_number = 1;
static volatile sig_atomic_t foreground_running = 0;
static volatile sig_atomic_t sigchld_received = 0;

static pid_t shell_pgid;
static int shell_terminal;

// Helper func to block/un-block sigchld!
static void block_sigchld(sigset_t *old_mask) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if(sigprocmask(SIG_BLOCK, &mask, old_mask) == -1) {
        perror("sigprocmask");
    }
}

static void unblock_sigchld(const sigset_t *old_mask) {
    if(sigprocmask(SIG_SETMASK, old_mask, NULL) == -1) {
        perror("sigprocmask");
    }
}

// SIGCHLD handler!
static void handle_sigchld(int signal) {
    (void)signal;
    sigchld_received = 1;
}

static void process_sigchld() {
    if(!sigchld_received) {
        return;
    }
    sigchld_received = 0;
    int status;
    for(int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
        if(!background_jobs[i].active) {
            continue;
        }
        // We check every process belonging to this job!
        for(size_t j = 0; j < background_jobs[i].process_count; j++) {
            // A pid of -1 indicates the process has already exited!
            if(background_jobs[i].pids[j] == -1) {
                continue;
            }
            pid_t pid = background_jobs[i].pids[j];
            while(true) {
                pid_t result = waitpid(pid, &status, WNOHANG | WUNTRACED); 
                if(result == 0) {
                    break;
                }
                if(result == -1) {
                    if(errno == EINTR) {
                        continue;
                    }
                    break;
                }
                // Process exited normally!
                if(WIFEXITED(status)) {
                    background_jobs[i].pids[j] = -1;
                }
                // Process was killed by a signal!
                else if(WIFSIGNALED(status)) {
                    background_jobs[i].normal = false;
                    background_jobs[i].pids[j] = -1;
                }
                // Process was stopped, for example, via Control+Z!
                else if(WIFSTOPPED(status)) {
                    background_jobs[i].states[j] = PROCESS_STOPPED;
                }
            }
        }
        // Determine whether every process in the job has exited or not!
        bool all_exited = true;
        for(size_t j = 0; j < background_jobs[i].process_count; j++) {
            if(background_jobs[i].pids[j] != -1) {
                all_exited = false;
                break;
            }
        }
        // The job is marked as completed only when every process in the job has exited!
        if(all_exited) {
            background_jobs[i].completed = true;
        }
    }
}

static int find_free_job_slot() {
    for(int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
        if(!background_jobs[i].active) {
            return i;
        }
    }
    return -1;
}

static bool add_background_job(pid_t pid, pid_t pgid, const pid_t *pids, size_t process_count, Token *tokens, const char *command, ProcessState initial_state, bool print_start) {
    int slot = find_free_job_slot();
    if(slot == -1) {
        fprintf(stderr, "cshell: too many background jobs\n");
        return false;
    }
    background_jobs[slot].pids = malloc(process_count * sizeof(pid_t));
    if(background_jobs[slot].pids == NULL) {
        return false;
    }
    background_jobs[slot].states = malloc(process_count * sizeof(ProcessState));
    if(background_jobs[slot].states == NULL) {
        free(background_jobs[slot].pids);
        background_jobs[slot].pids = NULL;
        return false;
    }
    background_jobs[slot].commands = malloc(process_count * sizeof(char *));
    if(background_jobs[slot].commands == NULL) {
        free(background_jobs[slot].states);
        free(background_jobs[slot].pids);
        background_jobs[slot].states = NULL;
        background_jobs[slot].pids = NULL;
        return false;
    }
    memcpy(background_jobs[slot].pids, pids, process_count * sizeof(pid_t));
    for(size_t i = 0; i < process_count; i++) {
        background_jobs[slot].states[i] = initial_state;
    }
    Token *current = tokens;
    for(size_t i = 0; i < process_count; i++) {
        background_jobs[slot].commands[i] = malloc(strlen(current->value) + 1);
        if(background_jobs[slot].commands[i] == NULL) {
            for(size_t j = 0; j < i; j++) {
                free(background_jobs[slot].commands[j]);
            }
            free(background_jobs[slot].commands);
            free(background_jobs[slot].states);
            free(background_jobs[slot].pids);
            background_jobs[slot].commands = NULL;
            background_jobs[slot].states = NULL;
            background_jobs[slot].pids = NULL;
            return false;
        }
        strcpy(background_jobs[slot].commands[i], current->value);
        if(i + 1 < process_count) {
            while(current != NULL && current->type != TOKEN_PIPE) {
                current = current->next;
            }
            if(current != NULL) {
                current = current->next;
            }
        }
    }
    background_jobs[slot].process_count = process_count;
    background_jobs[slot].pid = pid;
    background_jobs[slot].pgid = pgid;
    background_jobs[slot].job_number = next_job_number++;
    background_jobs[slot].active = true;
    background_jobs[slot].completed = false;
    background_jobs[slot].normal = true;
    snprintf(background_jobs[slot].command, sizeof(background_jobs[slot].command), "%s", command);
    if(print_start) {
        printf("[%lu] %d\n", background_jobs[slot].job_number, background_jobs[slot].pid);
    }
    return true;
}

// Helper function to print deferred completions!
void print_completed_background_jobs() {
    process_sigchld();
    for(int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
        if(!background_jobs[i].active || !background_jobs[i].completed) {
            continue;
        }
        if(background_jobs[i].normal) {
            printf("%s with pid %d exited normally\n", background_jobs[i].command, background_jobs[i].pid);
        }
        else {
            printf("%s with pid %d exited abnormally\n", background_jobs[i].command, background_jobs[i].pid);
        }
        // Freeing pid, states, commands arr after use!
        for(size_t j = 0; j < background_jobs[i].process_count; j++) {
            free(background_jobs[i].commands[j]);
        }
        free(background_jobs[i].commands);
        background_jobs[i].commands = NULL;
        free(background_jobs[i].pids);
        free(background_jobs[i].states);
        background_jobs[i].pids = NULL;
        background_jobs[i].states = NULL;
        background_jobs[i].process_count = 0;
        background_jobs[i].active = false;
        background_jobs[i].completed = false;
    }
}

void print_activities() {
    process_sigchld();
    for(unsigned long number = 1; number < next_job_number; number++) {
        for(int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
            if(!background_jobs[i].active) {
                continue;
            }
            if(background_jobs[i].job_number != number) {
                continue;
            }
            if(background_jobs[i].completed) {
                continue;
            }
            printf("[%lu] pgid %d\n", background_jobs[i].job_number, background_jobs[i].pgid);
            for(size_t j = 0; j < background_jobs[i].process_count; j++) {
                if(background_jobs[i].pids[j] == -1) {
                    continue;
                }
                const char *state;
                if(background_jobs[i].states[j] == PROCESS_RUNNING) {
                    state = "Running";
                }
                else {
                    state = "Stopped";
                }
                printf("    %d %s %s\n", background_jobs[i].pids[j], background_jobs[i].commands[j], state);
            }
        }
    }
}

bool has_stopped_jobs() {
    process_sigchld();
    for(int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
        if(!background_jobs[i].active || background_jobs[i].completed) {
            continue;
        }
        for(size_t j = 0; j < background_jobs[i].process_count; j++) {
            if(background_jobs[i].pids[j] != -1 && background_jobs[i].states[j] == PROCESS_STOPPED) {
                return true;
            }
        }
    }
    return false;
}

void shutdown_executor() {
    for(int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
        if(!background_jobs[i].active || background_jobs[i].completed) {
            continue;
        }
        if(background_jobs[i].pid > 0) {
            kill(-background_jobs[i].pgid, SIGHUP);
        }
    }
}

// Helper functions for terminal controls!
static void ignore_signal(int signal_number) {
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if(sigaction(signal_number, &sa, NULL) == -1) {
        perror("sigaction");
        _exit(EXIT_FAILURE);
    }
}

static void reset_child_signals() {
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if(sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTSTP, &sa, NULL) == -1 || sigaction(SIGTTOU, &sa, NULL) == -1) {
        _exit(EXIT_FAILURE);
    }
}

void initialise_executor(void) {
    shell_terminal = STDIN_FILENO;
    shell_pgid = getpid();
    ignore_signal(SIGINT);
    ignore_signal(SIGTSTP);
    ignore_signal(SIGTTOU);
    if(setpgid(shell_pgid, shell_pgid) == -1 && errno != EACCES) {
        perror("setpgid");
        exit(EXIT_FAILURE);
    }
    if(tcsetpgrp(shell_terminal, shell_pgid) == -1) {
        perror("tcsetpgrp");
        exit(EXIT_FAILURE);
    }
    struct sigaction sa;
    sa.sa_handler = handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if(sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

// Helper func to check if the file is exec or not!
static bool is_executable(const char *path) {
    struct stat information;
    // stat() func displays file/file system status!
    if(stat(path, &information) == -1) {
        return false;
    }
    if(!S_ISREG(information.st_mode)) {
        return false;
    }
    // Checks if callinf proccess can access the file path!
    return access(path, X_OK) == 0;
}

// We must allow exec of arbitrary commands, thus, need to resolve file paths!
static bool resolve_command(const char *command, char *resolved_path, size_t size) {
    // %name indicates to not search the cwd, and instead search the PATH directly!
    bool skip_current_directory = false;
    const char *name = command;
    if(command[0] == '%') {
        skip_current_directory = true;
        name = command + 1;
    }
    // An empty cmd after '%' is not exec!
    if(name[0] == '\0') {
        return false;
    }
    // If the cmd contains '/', it is an abs path!
    if(strchr(name, '/') != NULL) {
        if(!is_executable(name)) {
            return false;
        }
        int written = snprintf(resolved_path, size, "%s", name);
        return written >= 0 && (size_t)written < size;
    }
    // For ordinary cmd, check the cwd first unless '%' was specified!
    if(!skip_current_directory) {
        char current_directory[PATH_MAX];
        if(getcwd(current_directory, sizeof(current_directory)) != NULL) {
            char candidate[PATH_MAX];
            int written = snprintf(candidate, sizeof(candidate), "%s/%s", current_directory, name);
            if(written >= 0 && (size_t)written < sizeof(candidate) && is_executable(candidate)) {
                int copied = snprintf(resolved_path, size, "%s", candidate);
                return copied >= 0 && (size_t)copied < size;
            }
        }
    }
    // Searching every dir in PATH in order!
    const char *path = getenv("PATH");
    if(path == NULL) {
        return false;
    }
    char *path_copy = malloc(strlen(path) + 1);
    if(path_copy == NULL) {
        return false;
    }
    strcpy(path_copy, path);
    char *directory = path_copy;
    while(true) {
        char *separator = strchr(directory, ':');
        if(separator != NULL) {
            *separator = '\0';
        }
        // Empty path comp indicates the cwd!
        char candidate[PATH_MAX];
        int written;
        if(directory[0] == '\0') {
            char current_directory[PATH_MAX];
            if(getcwd(current_directory, sizeof(current_directory)) == NULL) {
                free(path_copy);
                return false;
            }
            written = snprintf(candidate, sizeof(candidate), "%s/%s", current_directory, name);
        }
        else {
            written = snprintf(candidate, sizeof(candidate), "%s/%s", directory, name);
        }
        if(written >= 0 && (size_t)written < sizeof(candidate) && is_executable(candidate)) {
            int copied = snprintf(resolved_path, size, "%s", candidate);
            free(path_copy);
            return copied >= 0 && (size_t)copied < size;
        }
        if(separator == NULL) {
            break;
        }
        directory = separator + 1;
    }
    free(path_copy);
    return false;
}

// Building the argv array - counting num of arg after the cmd!
static size_t count_command_arguments(Token *tokens) {
    size_t count = 0;
    Token *current = tokens;
    while(current != NULL) {
        if(current->type != TOKEN_WORD) {
            break;
        }
        count++;
        current = current->next;
    }
    return count;
}

static char **build_argv(Token *tokens) {
    size_t argument_count = count_command_arguments(tokens);
    char **argv = malloc((argument_count + 1) * sizeof(char *));
    if(argv == NULL) {
        return NULL;
    }
    Token *current = tokens;
    for(size_t i = 0; i < argument_count; i++) {
        argv[i] = current->value;
        current = current->next;
    }
    argv[argument_count] = NULL;
    return argv;
}

// Helper func to cnt the num of input redirections!
static size_t count_input_redirections(Token *tokens) {
    size_t count = 0;
    Token *current = tokens;
    while(current != NULL) {
        if(current->type == TOKEN_LT) {
            count++;
        }
        // We're concerned with the first cmd grp, hence, we stop before seeing a ;, &, or |!
        if(current->type == TOKEN_SEMI || current->type == TOKEN_AMP || current->type == TOKEN_PIPE) {
            break;
        }
        current = current->next;
    }
    return count;
}

// Opening the input files for the redirection!
static bool open_input_files(Token *tokens, int **input_fds, size_t *input_count) {
    size_t count = count_input_redirections(tokens);
    *input_count = count;
    *input_fds = NULL;
    if(count == 0) {
        return true;
    }
    int *fds = malloc(count * sizeof(int));
    if(fds == NULL) {
        return false;
    }
    size_t index = 0;
    Token *current = tokens;
    while(current != NULL && index < count) {
        if(current->type == TOKEN_LT) {
            Token *filename = current->next;
            int fd = open(filename->value, O_RDONLY);
            if(fd == -1) {
                for(size_t i = 0; i < index; i++) {
                    close(fds[i]);
                }
                free(fds);
                printf("cshell: no such file or directory\n");
                return false;
            }
            fds[index] = fd;
            index++;
        }
        if(current->type == TOKEN_SEMI || current->type == TOKEN_AMP || current->type == TOKEN_PIPE) {
            break;
        }
        current = current->next;
    }
    *input_fds = fds;
    return true;
}

// Parent will concat all the files!
static bool write_all(int fd, const char *buffer, size_t count) {
    size_t written = 0;
    while(written < count) {
        ssize_t result = write(fd, buffer + written, count - written);
        if(result < 0) {
            if(errno == EINTR) {
                continue;
            }
            return false;
        }
        if(result == 0) {
            return false;
        }
        written += (size_t)result;
    }
    return true;
}

// We'll copy the file into pipe, then change the fd for stdin to one end of the pipe!
static bool copy_file_to_pipe(int input_fd, int pipe_fd) {
    char buffer[4096];
    while(true) {
        ssize_t bytes_read = read(input_fd, buffer, sizeof(buffer));
        if(bytes_read == 0) {
            return true;
        }
        if(bytes_read < 0) {
            if(errno == EINTR) {
                continue;
            }
            return false;
        }
        if(!write_all(pipe_fd, buffer, (size_t)bytes_read)) {
            return false;
        }
    }
}

// Helper func to count the num of output redir!
static size_t count_output_redirections(Token *tokens) {
    size_t count = 0;
    Token *current = tokens;
    while(current != NULL) {
        if(current->type == TOKEN_GT || current->type == TOKEN_GTGT) {
            count++;
        }
        // Again, we're only concerned with the first cmd grp!
        if(current->type == TOKEN_SEMI || current->type == TOKEN_AMP || current->type == TOKEN_PIPE) {
            break;
        }
        current = current->next;
    }
    return count;
}

// Opening all output files for redir!
static bool open_output_files(Token *tokens, int **output_fds, size_t *output_count) {
    size_t count = count_output_redirections(tokens);
    *output_count = count;
    *output_fds = NULL;
    if(count == 0) {
        return true;
    }
    int *fds = malloc(count * sizeof(int));
    if(fds == NULL) {
        return false;
    }
    size_t index = 0;
    Token *current = tokens;
    while(current != NULL && index < count) {
        if(current->type == TOKEN_GT || current->type == TOKEN_GTGT) {
            Token *filename = current->next;
            int flags;
            // > overwrites the cont of the file, while >> simply appends to the file!
            // Incase the file does not exist, we must creat it!
            if(current->type == TOKEN_GT) {
                flags = O_WRONLY | O_CREAT | O_TRUNC;
            }
            else {
                flags = O_WRONLY | O_CREAT | O_APPEND;
            }
            int fd = open(filename->value, flags, 0644);
            if(fd == -1) {
                for(size_t i = 0; i < index; i++) {
                    close(fds[i]);
                }
                free(fds);
                printf("cshell: unable to create the file for writing\n");
                return false;                
            }
            fds[index] = fd;
            index++;
        }
        if(current->type == TOKEN_SEMI || current->type == TOKEN_AMP || current->type == TOKEN_PIPE) {
            break;
        }
        current = current->next;
    }
    *output_fds = fds;
    return true;
}

// Writing the same data to every output redir targ!
static bool write_to_all_outputs(int *output_fds, size_t output_count, const char *buffer, size_t count) {
    for(size_t i = 0; i < output_count; i++) {
        if(!write_all(output_fds[i], buffer, count)) {
            return false;
        }
    }
    return true;
}

// Func to cnt num of pipe cmd!
static size_t count_pipeline_commands(Token *tokens) {
    size_t count = 1;
    Token *current = tokens;
    while(current != NULL) {
        if(current->type == TOKEN_PIPE) {
            count++;
        }
        // Again, we're only concerned with the first cmd grp!
        if(current->type == TOKEN_SEMI || current->type == TOKEN_AMP) {
            break;
        }
        current = current->next;
    }
    return count;
}

// This func ret the token at the start of the pipeline stage!
static Token *get_pipeline_stage(Token *tokens, size_t stage_number) {
    Token *current = tokens;
    size_t current_stage = 0;
    while(current != NULL && current_stage < stage_number) {
        if(current->type == TOKEN_PIPE) {
            current_stage++;
        }
        current = current->next;
    }
    return current;
}

// Exec the pipeline of ext cmd!
static bool execute_pipeline(Token *tokens, bool background) {
    size_t command_count = count_pipeline_commands(tokens);
    // Pipeline req atleast two cmd!
    if(command_count < 2) {
        return false;
    }
    pid_t pgid = -1;
    sigset_t old_mask;
    if(background) {
        block_sigchld(&old_mask);
    }

    // A pipeline containing N cmd needs N - 1 pipes!
    int (*pipes)[2] = malloc((command_count - 1) * sizeof(int[2]));

    // This is to handle multi output redir!
    int (*output_pipes)[2] = malloc(command_count * sizeof(int[2]));
    // Malloc failed!
    if(pipes == NULL || output_pipes == NULL) {
        free(pipes);
        free(output_pipes);
        return false;
    }
    for(size_t i = 0; i < command_count; i++) {
        output_pipes[i][0] = -1;
        output_pipes[i][1] = -1;
    }
    
    // Creat all pipes before forking!
    for(size_t i = 0; i < command_count - 1; i++) {
        if(pipe(pipes[i]) == -1) {
            perror("pipe");
            for(size_t j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);                
            }
            free(pipes);
            return false;
        }
    }
    // Store the child pids, so that the parent can wait for every single child!
    pid_t *children = malloc(command_count * sizeof(pid_t));
    if(children == NULL) {
        for(size_t i = 0; i < command_count - 1; i++) {
            close(pipes[i][0]);
            close(pipes[i][1]);
        }
        free(pipes);
        return false;
    }

    pid_t *output_writers = malloc(command_count * sizeof(pid_t));
    if(output_writers == NULL) {
        for(size_t i = 0; i < command_count - 1; i++) {
            close(pipes[i][0]);
            close(pipes[i][1]);
        }
        free(children);
        free(pipes);
        return false;
    }

    for(size_t i = 0; i < command_count; i++) {
        children[i] = -1;
        output_writers[i] = -1;
    }

    for(size_t i = 0; i < command_count; i++) {
        Token *stage = get_pipeline_stage(tokens, i);
        
        // Need to implement the func to check is it's a built-in cmd!
        bool is_builtin = is_builtin_command(stage);
        char resolved_path[PATH_MAX];
        if(!is_builtin) {            
            // Resolve the exec for this particular stage!
            if(!resolve_command(stage->value, resolved_path, sizeof(resolved_path))) {
                // This stage is allowed to fail while the rem pipeline continues!
                printf("cshell: command not found (%s)\n", stage->value[0] == '%' ? stage->value + 1 : stage->value);
                // children[i] = -1;
                continue;
            }
        }

        char **argv = build_argv(stage);
        if(argv == NULL) {
            // children[i] = -1;
            continue;
        }

        int *input_fds = NULL;
        size_t input_count = 0;
        if(!open_input_files(stage, &input_fds, &input_count)) {
            free(argv);
            continue;
        }
        int *output_fds = NULL;
        size_t output_count = 0;
        if(!open_output_files(stage, &output_fds, &output_count)) {
            // Closing all open file desc if this func call fails!
            for(size_t i = 0; i < input_count; i++) {
                close(input_fds[i]);
            }

            free(input_fds);
            free(argv);
            continue;
        }

        // Multi-output redir!
        if(output_count > 1) {
            if(pipe(output_pipes[i]) == -1) {
                perror("pipe");
                for(size_t j = 0; j < input_count; j++) {
                    close(input_fds[j]);
                }
                for(size_t j = 0; j < output_count; j++) {
                    close(output_fds[j]);
                }
                free(input_fds);
                free(output_fds);
                free(argv);
                continue;
            }
        }

        pid_t child = fork();
        if(child < 0) {
            perror("fork");
            for(size_t j = 0; j < input_count; j++) {
                close(input_fds[j]);
            }
            for(size_t j = 0; j < output_count; j++) {
                close(output_fds[j]);
            }
            free(input_fds);
            free(output_fds);
            free(argv);
            // children[i] = -1;
            continue;                   
        }
        children[i] = child;
        // Child procc!
        if(child == 0) {
            reset_child_signals();
            if(i == 0) {
                if(setpgid(0, 0) == -1) {
                    perror("setpgid");
                    _exit(EXIT_FAILURE);
                }
            }
            else {
                if(setpgid(0, pgid) == -1) {
                    perror("setpgid");
                    _exit(EXIT_FAILURE);
                }
            }

            // Background proc handling!
            if(background && input_count == 0 && i == 0) {
                int null_fd = open("/dev/null", O_RDONLY);
                if(null_fd == -1) {
                    _exit(EXIT_FAILURE);
                }
                if(dup2(null_fd, STDIN_FILENO) == -1) {
                    close(null_fd);
                    _exit(EXIT_FAILURE);
                }
                close(null_fd);
            }

            // Explicit input redir has more precedence than pipline input!
            if(input_count == 1) {
                if(dup2(input_fds[0], STDIN_FILENO) == -1) {
                    perror("dup2");
                    _exit(EXIT_FAILURE);
                }
            }

            // If this isn't the first cmd, its stdin comes from prev pipe!
            else if(input_count == 0 && i > 0) {
                if(dup2(pipes[i - 1][0], STDIN_FILENO) == -1) {
                    perror("dup2");
                    _exit(EXIT_FAILURE);
                }
            }            
            else if(input_count > 1) {
                int input_pipe[2];
                if(pipe(input_pipe) == -1) {
                    perror("pipe");
                    _exit(EXIT_FAILURE);
                }
                pid_t writer = fork();
                if(writer == -1) {
                    perror("fork");
                    _exit(EXIT_FAILURE);
                }
                if(writer == 0) {
                    for(size_t j = 0; j < command_count - 1; j++) {
                        close(pipes[j][0]);
                        close(pipes[j][1]);
                    }
                    close(input_pipe[0]);
                    for(size_t j = 0; j < input_count; j++) {
                        if(!copy_file_to_pipe(input_fds[j], input_pipe[1])) {
                            close(input_pipe[1]);
                            _exit(EXIT_FAILURE);
                        }
                    }
                    close(input_pipe[1]);
                    _exit(EXIT_SUCCESS);
                }
                close(input_pipe[1]);
                if(dup2(input_pipe[0], STDIN_FILENO) == -1) {
                    perror("dup2");
                    _exit(EXIT_FAILURE);
                }
                close(input_pipe[0]);
            }

            // Handling multi output redir!
            if(output_count == 1) {
                if(dup2(output_fds[0], STDOUT_FILENO) == -1) {
                    perror("dup2");
                    _exit(EXIT_FAILURE);
                }
            }
            else if(output_count > 1) {
                if(dup2(output_pipes[i][1], STDOUT_FILENO) == -1) {
                    perror("dup2");
                    _exit(EXIT_FAILURE);
                }
            }
            else if(i < command_count - 1) {
                if(dup2(pipes[i][1], STDOUT_FILENO) == -1) {
                    perror("dup2");
                    _exit(EXIT_FAILURE);                    
                }
            }

            // Close every pipe desc!
            for(size_t j = 0; j < command_count - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            for(size_t j = 0; j < command_count; j++) {
                if(output_pipes[j][0] != -1) {
                    close(output_pipes[j][0]);
                }
                if(output_pipes[j][1] != -1) {
                    close(output_pipes[j][1]);
                }
            }

            // Close redir desc!
            for(size_t j = 0; j < input_count; j++) {
                close(input_fds[j]);
            }
            for(size_t j = 0; j < output_count; j++) {
                close(output_fds[j]);
            }
            free(input_fds);
            free(output_fds);

            // Handle the built-in cmd separately!
            if(is_builtin) {
                execute_builtin(stage);
                _exit(EXIT_SUCCESS);
            }

            execv(resolved_path, argv);
            _exit(EXIT_FAILURE);
        }
        // Parent assigns all pipeline children to the same process group!
        if(child > 0) {
            if(i == 0) {
                pgid = child;
            }
            if(setpgid(child, pgid) == -1) {
                perror("setpgid");
            }
        } 

        pid_t output_writer = -1;
        if(output_count > 1) {
            output_writer = fork();
            if(output_writer == -1) {
                perror("fork");
            }
            else if(output_writer == 0) {
                close(output_pipes[i][1]);
                // Closing all norm pipeline desc!
                for(size_t j = 0; j < command_count - 1; j++) {
                    close(pipes[j][0]);
                    close(pipes[j][1]);
                }
                // Closing every othr output pipe!
                for(size_t j = 0; j < command_count; j++) {
                    if(j != i && output_pipes[j][0] != -1) {
                        close(output_pipes[j][0]);
                    }
                    if(j != i && output_pipes[j][1] != -1) {
                        close(output_pipes[j][1]);
                    }
                }
                // Copy the cmd output to every file!
                char buffer[4096];
                while(true) {
                    ssize_t bytes_read = read(output_pipes[i][0], buffer, sizeof(buffer));
                    if(bytes_read == 0) {
                        break;
                    }
                    if(bytes_read < 0) {
                        if(errno == EINTR) {
                            continue;
                        }
                        _exit(EXIT_FAILURE);
                    }
                    if(!write_to_all_outputs(output_fds, output_count, buffer, (size_t)bytes_read)) {
                        _exit(EXIT_FAILURE);
                    }
                }
                close(output_pipes[i][0]);
                for(size_t j = 0; j < output_count; j++) {
                    close(output_fds[j]);
                }
                _exit(EXIT_SUCCESS);
            }

            else {
                output_writers[i] = output_writer;
                close(output_pipes[i][0]);
                close(output_pipes[i][1]);
            }
            for(size_t j = 0; j < output_count; j++) {
                close(output_fds[j]);
            }
            free(output_fds);
        }
    }
    // Parent must close every pipe desc it holds!
    for(size_t i = 0; i < command_count - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    if(background) {
        char command_string[4096];
        build_command_string(tokens, command_string, sizeof(command_string));
        if(children[0] != -1) {
            if(!add_background_job(children[0], pgid, children, command_count, tokens, command_string, PROCESS_RUNNING, true)) {
                for(size_t i = 0; i < command_count; i++) {
                    if(children[i] != -1) {
                        kill(children[i], SIGTERM);
                    }
                }
            }
        }
        unblock_sigchld(&old_mask);
    }
    if(!background) {
        foreground_running = 1;
        bool pipeline_stopped = false;
        bool pipeline_signaled = false;
        // Give the terminal to the entire foreground pipeline!
        if(tcsetpgrp(shell_terminal, pgid) == -1) {
            perror("tcsetpgrp");
        }
        for(size_t i = 0; i < command_count; i++) {
            if(children[i] != -1) {
                int status;
                while(waitpid(children[i], &status, WUNTRACED) == -1) {
                    if(errno == EINTR) {
                        continue;
                    }
                    break;
                }
                if(WIFSTOPPED(status)) {
                    pipeline_stopped = true;
                }
                if(WIFSIGNALED(status)) {
                    pipeline_signaled = true;
                }
            }
            // Only wait for output writers if the pipeline itself was not stopped!
            if(!pipeline_stopped && output_writers[i] != -1) {
                int status;
                while(waitpid(output_writers[i], &status, WUNTRACED) == -1) {
                    if(errno == EINTR) {
                        continue;
                    }
                    break;
                }
            }
        }
        // Take the terminal back after the pipeline finishes!
        if(tcsetpgrp(shell_terminal, shell_pgid) == -1) {
            perror("tcsetpgrp");
        }
        if(pipeline_signaled) {
            printf("\n");
        }
        foreground_running = 0;
        if(pipeline_stopped) {
            printf("\n");
            char command_string[4096];
            build_command_string(tokens, command_string, sizeof(command_string));
            pid_t first_pid = children[0];
            if(add_background_job(first_pid, pgid, children, command_count, tokens, command_string, PROCESS_STOPPED, false)) {
                for(int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
                    if(background_jobs[i].active && background_jobs[i].pid == first_pid) {
                        printf("[%lu] + Stopped %s\n", background_jobs[i].job_number, command_string);
                        break;
                    }
                }
            }
        }
        print_completed_background_jobs();
    }
    free(output_writers);
    free(children);
    free(output_pipes);
    free(pipes);
    return true;
}

static void build_command_string(Token *tokens, char *buffer, size_t buffer_size) {
    buffer[0] = '\0';
    size_t used = 0;
    for(Token *current = tokens; current != NULL; current = current->next) {
        // Stop at the end of command sequence!
        if(current->type == TOKEN_SEMI || current->type == TOKEN_AMP) {
            break;
        }
        size_t remaining = buffer_size - used;
        if(remaining <= 1) {
            break;
        }
        int written;
        if(current->type == TOKEN_PIPE) {
            written = snprintf(buffer + used, remaining, " | ");
        }
        else {
            written = snprintf(buffer + used, remaining, "%s%s", used == 0 ? "" : " ", current->value);
        }
        if(written < 0) {
            buffer[0] = '\0';
            return;
        }
        if((size_t)written > remaining) {
            buffer[buffer_size - 1] = '\0';
            return;
        }
        used += (size_t)written;
    }
    // Remove trailing white-spaces!
    while(used > 0 && buffer[used - 1] == ' ') {
        buffer[--used] = '\0';
    }
}

// We now fork() and ask the child to exec the cmd!
static bool execute_external(Token *tokens, bool background) {
    char resolved_path[PATH_MAX];
    if(!resolve_command(tokens->value, resolved_path, sizeof(resolved_path))) {
        printf("cshell: command not found (%s)\n", tokens->value[0] == '%' ? tokens->value + 1 : tokens->value);
        return false;
    }
    char **argv = build_argv(tokens);
    if(argv == NULL) {
        return false;
    }

    // Build command string for background job reporting!
    char command_string[4096];
    build_command_string(tokens, command_string, sizeof(command_string));

    // Input redir!
    int *input_fds = NULL;
    size_t input_count = 0;
    if(!open_input_files(tokens, &input_fds, &input_count)) {
        free(argv);
        return false;
    }

    // Output redir!
    int *output_fds = NULL;
    size_t output_count = 0;
    if(!open_output_files(tokens, &output_fds, &output_count)) {
        free(input_fds);
        free(argv);
        return false;
    }

    // Input pipe!
    int input_pipe[2] = {-1, -1};
    if(input_count > 0) {
        if(pipe(input_pipe) == -1) {
            perror("pipe");
            for(size_t i = 0; i < input_count; i++) {
                close(input_fds[i]);
            }
            free(input_fds);
            free(output_fds);
            free(argv);
            return false;
        }
    }

    // Output pipe!
    int output_pipe[2] = {-1, -1};
    if(output_count > 0) {
        if(pipe(output_pipe) == -1) {
            perror("pipe");
            if(input_count > 0) {
                close(input_pipe[0]);
                close(input_pipe[1]);
            }
            for(size_t i = 0; i < input_count; i++) {
                close(input_fds[i]);
            }
            for(size_t i = 0; i < output_count; i++) {
                close(output_fds[i]);
            }
            free(input_fds);
            free(output_fds);
            free(argv);
            return false;
        }
    }

    sigset_t old_mask;
    if(background) {
        block_sigchld(&old_mask);
    }

    pid_t child = fork();
    if(child < 0) {
        perror("fork");
        if(input_count > 0) {
            close(input_pipe[0]);
            close(input_pipe[1]);
        }
        if(output_count > 0) {
            close(output_pipe[0]);
            close(output_pipe[1]);
        }
        for(size_t i = 0; i < input_count; i++) {
            close(input_fds[i]);
        }
        for(size_t i = 0; i < output_count; i++) {
            close(output_fds[i]);
        }
        free(input_fds);
        free(output_fds);
        free(argv);
        return false;
    }
    if(child == 0) {
        reset_child_signals();
        if(setpgid(0, 0) == -1) {
            perror("setpgid");
            _exit(EXIT_FAILURE);
        }

        if(background) {
            unblock_sigchld(&old_mask);
        }

        // Redir stdin!
        if(input_count > 0) {
            close(input_pipe[1]);
            if(dup2(input_pipe[0], STDIN_FILENO) == -1) {
                perror("dup2");
                _exit(EXIT_FAILURE);
            }
            close(input_pipe[0]);            
        }        

        // Redir stdout!
        if(output_count > 0) {
            close(output_pipe[0]);
            if(dup2(output_pipe[1], STDOUT_FILENO) == -1) {
                perror("dup2");
                _exit(EXIT_FAILURE);
            }
            close(output_pipe[1]);
        }

        if(background && input_count == 0) {
            int null_fd = open("/dev/null", O_RDONLY);
            if(null_fd == -1) {
                _exit(EXIT_FAILURE);
            }
            if(dup2(null_fd, STDIN_FILENO) == -1) {
                close(null_fd);
                _exit(EXIT_FAILURE);
            }
            close(null_fd);
        }

        // The child no longer requires the file desc!
        for(size_t i = 0; i < input_count; i++) {
            close(input_fds[i]);
        }
        for(size_t i = 0; i < output_count; i++) {
            close(output_fds[i]);
        }
        execv(resolved_path, argv);
        // We reach here only if execv() failed!
        perror("exec");
        _exit(EXIT_FAILURE);
    }

    // Parent code!
    if(setpgid(child, child) == -1) {
        perror("setpgid");
    }

    // Parent waits for the child to complete exec!
    if(input_count > 0) {
        close(input_pipe[0]);
        bool write_success = true;
        for(size_t i = 0; i < input_count; i++) {
            if(!copy_file_to_pipe(input_fds[i], input_pipe[1])) {
                write_success = false;
                break;
            }
        }
        close(input_pipe[1]);
        for(size_t i = 0; i < input_count; i++) {
            close(input_fds[i]);
        }
        free(input_fds);
        // Again, to prevent unused var warn!
        (void)write_success;
    }
    if(output_count > 0) {
        close(output_pipe[1]);
        char buffer[4096];
        while(true) {
            ssize_t bytes_read = read(output_pipe[0], buffer, sizeof(buffer));
            if(bytes_read == 0) {
                break;
            }
            if(bytes_read < 0) {
                if(errno == EINTR) {
                    continue;
                }
                break;
            }
            if(!write_to_all_outputs(output_fds, output_count, buffer, (size_t)bytes_read)) {
                break;
            }
        }
        close(output_pipe[0]);
        for(size_t i = 0; i < output_count; i++) {
            close(output_fds[i]);
        }
        free(output_fds);
    }
    if(background) {
        if(!add_background_job(child, child, &child, 1, tokens, command_string, PROCESS_RUNNING, true)) {
            kill(child, SIGTERM);
        }
        unblock_sigchld(&old_mask);
    }   
    else {
        int status;
        foreground_running = 1;
        // Give the terminal the foreground process group!
        if(tcsetpgrp(shell_terminal, child) == -1) {
            perror("tcsetpgrp");
        }
        while(waitpid(child, &status, WUNTRACED) == -1) {
            if(errno == EINTR) {
                continue;
            }
            break;
        }
        // Take terminal back after the command finishes!
        if(tcsetpgrp(shell_terminal, shell_pgid) == -1) {
            perror("tcsetpgrp");
        }
        if(WIFSIGNALED(status)) {
            // Move to a new line after Control+C is printed as ^C!
            printf("\n");
        }
        foreground_running = 0;
        if(WIFSTOPPED(status)) {
            // Move to a new line after Control+Z is printed as ^Z!
            printf("\n");
            // The foreground process was stopped using Control+Z!
            if(add_background_job(child, child, &child, 1, tokens, command_string, PROCESS_STOPPED, false)) {
                // Find the job we just created and print the message!
                for(int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
                    if(background_jobs[i].active && background_jobs[i].pid == child) {
                        printf("[%lu] + Stopped %s\n", background_jobs[i].job_number, command_string);
                        break;
                    }
                }
            }
        }
        print_completed_background_jobs();
    }

    free(argv);
    return true;
}

bool execute_command(Token *tokens, bool background) {
    if(tokens == NULL || tokens->type != TOKEN_WORD) {
        return false;
    }
    Token *current = tokens;
    while(current != NULL) {
        if(current->type == TOKEN_PIPE) {
            return execute_pipeline(tokens, background);
        }
        if(current->type == TOKEN_SEMI || current->type == TOKEN_AMP) {
            break;
        }
        current = current->next;
    }
    return execute_external(tokens, background);
}