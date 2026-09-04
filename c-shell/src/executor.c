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
typedef struct {
    pid_t pid;
    unsigned long job_number;
    char command[4096];
    bool active;
} BackgroundJob;
static BackgroundJob background_jobs[MAX_BACKGROUND_PROCESSES];
static unsigned long next_job_number = 1;

// Maintain a small array of processes launched in the background!
// static pid_t background_pids[MAX_BACKGROUND_PROCESSES];
// static size_t background_count = 0;

// Helper functions for background processes!
// static bool is_background_pid(pid_t pid) {
//     for(size_t i = 0; i < background_count; i++) {
//         if(background_pids[i] == pid) {
//             return true;
//         }
//     }
//     return false;
// }

// static void add_background_pid(pid_t pid) {
//     if(background_count < MAX_BACKGROUND_PROCESSES) {
//         background_pids[background_count] = pid;
//         background_count++;
//     }
// }

// SIGCHLD handler!
static void handle_sigchld(int signal) {
    (void)signal;
    int status;
    pid_t pid;
    while((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for(int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
            if(!background_jobs[i].active || background_jobs[i].pid != pid) {
                continue;
            }
            char message[4096];
            if(WIFEXITED(status)) {
                snprintf(message, sizeof(message), "\n%s with pid %d exited normally\n", background_jobs[i].command, pid);
            }
            else if(WIFSIGNALED(status)) {
                snprintf(message, sizeof(message), "\n%s with pid %d exited abnormally\n", background_jobs[i].command, pid);
            }
            else {
                break;
            }
            write(STDOUT_FILENO, message, strlen(message));
            background_jobs[i].active = false;
            break;
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

static bool add_background_job(pid_t pid, const char *command) {
    int slot = find_free_job_slot();
    if(slot == -1) {
        fprintf(stderr, "cshell: too many background jobs\n");
        return false;
    }
    background_jobs[slot].pid = pid;
    background_jobs[slot].job_number = next_job_number++;
    background_jobs[slot].active = true;
    snprintf(background_jobs[slot].command, sizeof(background_jobs[slot].command), "%s", command);
    printf("[%lu] %d\n", background_jobs[slot].job_number, background_jobs[slot].pid);
    return true;
}

void initialise_executor(void) {
    struct sigaction sa;
    sa.sa_handler = handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
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

// Re-directing stdin!
// static bool redirect_input(int *input_fds, size_t input_count) {
//     if(input_count == 0) {
//         return true;
//     }
//     int input_pipe[2];
//     if(pipe(input_pipe) == -1) {
//         perror("pipe");
//         return false;
//     }
//     pid_t writer = fork();
//     if(writer == -1) {
//         perror("fork");
//         close(input_pipe[0]);
//         close(input_pipe[1]);
//         return false;
//     }
//     if(writer == 0) {
//         // This child only writes the concat input!
//         close(input_pipe[0]);
//         for(size_t i = 0; i < input_count; i++) {
//             if(!copy_file_to_pipe(input_fds[i], input_pipe[1])) {
//                 close(input_pipe[1]);
//                 for(size_t j = 0; j < input_count; j++) {
//                     close(input_fds[j]);
//                 }
//                 _exit(EXIT_FAILURE);
//             }
//         }
//         close(input_pipe[1]);
//         for(size_t i = 0; i < input_count; i++) {
//             close(input_fds[i]);
//         }
//         _exit(EXIT_SUCCESS);
//     }
//     // The process that called redirect_input() needs to have the read end as its stdin!
//     close(input_pipe[1]);
//     if(dup2(input_pipe[0], STDIN_FILENO) == -1) {
//         perror("dup2");
//         close(input_pipe[0]);
//         return false;
//     }
//     close(input_pipe[0]);
//     for(size_t i = 0; i < input_count; i++) {
//         close(input_fds[i]);
//     }
//     // The writer terminates alone once all input has been copied!
//     waitpid(writer, NULL, 0);
//     return true;
// }

// Creating the input pipe!
// static bool create_input_pipe(int *input_fds, size_t input_count, int input_pipe[2]) {
//     if(pipe(input_pipe) == -1) {
//         perror("pipe");
//         return false;
//     }
//     // Again, since we've used -Werror flag, we'll get the unused var warning - this avoids them!
//     (void)input_fds;
//     (void)input_count;
//     return true;
// }

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

// Cnt the num of tokens belonging to each stage!
// static size_t count_pipeline_stage_tokens(Token *stage) {
//     size_t count = 0;
//     Token *current = stage;
//     while(current != NULL) {
//         if(current->type == TOKEN_PIPE || current->type == TOKEN_SEMI || current->type == TOKEN_AMP) {
//             break;
//         }
//         count++;
//         current = current->next;
//     }
//     return count;
// }

// Exec the pipeline of ext cmd!
static bool execute_pipeline(Token *tokens) {
    size_t command_count = count_pipeline_commands(tokens);
    // Pipeline req atleast two cmd!
    if(command_count < 2) {
        return false;
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
    // Wait for every successfully creat child!
    for(size_t i = 0; i < command_count; i++) {
        if(children[i] != -1) {
            int status;
            if(waitpid(children[i], &status, 0) == -1) {
                perror("waitpid");                
            }
        }
        if(output_writers[i] != -1) {
            int status;
            if(waitpid(output_writers[i], &status, 0) == -1) {
                perror("waitpid");
            }
        }
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
    for(Token *current = tokens; current != NULL && current->type == TOKEN_WORD; current = current->next) {
        size_t remaining = buffer_size - used;
        if(remaining <= 1) {
            break;
        }
        int written = snprintf(buffer + used, remaining, "%s%s", used == 0 ? "" : " ", current->value);
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

    // Background execution handling!
    char command_string[4096];
    build_command_string(tokens, command_string, sizeof(command_string));
    if(background) {
        pid_t child = fork();
        if(child < 0) {
            perror("fork");
            free(argv);
            return false;
        }
        if(child == 0) {
            execv(resolved_path, argv);
            perror("exec");
            _exit(EXIT_FAILURE);
        }
        if(!add_background_job(child, command_string)) {
            waitpid(child, NULL, 0);
        }
        free(argv);
        return true;
    }

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
    char command_string[4096];
    build_command_string(tokens, command_string, sizeof(command_string));

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
    if(!background) {
        int status;
        if(waitpid(child, &status, 0) == -1) {
            perror("waitpid");
        }
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
            return execute_pipeline(tokens);
        }
        if(current->type == TOKEN_SEMI || current->type == TOKEN_AMP) {
            break;
        }
        current = current->next;
    }
    return execute_external(tokens, background);
}