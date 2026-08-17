#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include "executor.h"

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
    if(name[0] == '%') {
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

// We now fork() and ask the child to exec the cmd!
static bool execute_external(Token *tokens) {
    char resolved_path[PATH_MAX];
    if(!resolve_command(tokens->value, resolved_path, sizeof(resolved_path))) {
        printf("cshell: command not found (%s)\n", tokens->value[0] == '%' ? tokens->value + 1 : tokens->value);
        return false;
    }
    char **argv = build_argv(tokens);
    if(argv == NULL) {
        return false;
    }
    pid_t child = fork();
    if(child < 0) {
        perror("fork");
        free(argv);
        return false;
    }
    if(child == 0) {
        execv(resolved_path, argv);
        // We reach here only if execv() failed!
        perror("exec");
        _exit(EXIT_FAILURE);
    }
    // Parent waits for the child to complete exec!
    int status;
    if(waitpid(child, &status, 0) == -1) {
        perror("waitpid");
    }
    free(argv);
    return true;
}

bool execute_command(Token *tokens) {
    if(tokens == NULL || tokens->type != TOKEN_WORD) {
        return false;
    }
    return execute_external(tokens);
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