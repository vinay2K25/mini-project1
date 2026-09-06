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
#include "snoop.h"
extern char **environ;

static bool is_executable_file(const char *path) {
    struct stat information;
    if(stat(path, &information) == -1) {
        return false;
    }
    if(!S_ISREG(information.st_mode)) {
        return false;
    }
    return access(path, X_OK) == 0;
}

static bool resolve_snoop_command(const char *command, char *resolved_path, size_t size) {
    if(command == NULL || command[0] == '\0') {
        return false;
    }
    if(strchr(command, '/') != NULL) {
        if(!is_executable_file(command)) {
            return false;
        }
        int written = snprintf(resolved_path, size, "%s", command);
        return written >= 0 && (size_t)written < size;
    }
    char current_directory[PATH_MAX];
    if(getcwd(current_directory, sizeof(current_directory)) != NULL) {
        char candidate[PATH_MAX];
        int written = snprintf(candidate, sizeof(candidate), "%s/%s", current_directory, command);
        if(written >= 0 && (size_t)written < sizeof(candidate) && is_executable_file(candidate)) {
            int copied = snprintf(resolved_path, size, "%s", command);
            return copied >= 0 && (size_t)copied < size;
        }
    }
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
        char candidate[PATH_MAX];
        if(directory[0] == '\0') {
            if(getcwd(current_directory, sizeof(current_directory)) == NULL) {
                free(path_copy);
                return false;
            }
            int written = snprintf(candidate, sizeof(candidate), "%s/%s", current_directory, command);
            if(written < 0 || (size_t)written >= sizeof(candidate)) {
                free(path_copy);
                return false;
            }
        }
        else {
            int written = snprintf(candidate, sizeof(candidate), "%s/%s", directory, command);
            if(written < 0 || (size_t)written >= sizeof(candidate)) {
                free(path_copy);
                return false;
            }
        }
        if(is_executable_file(candidate)) {
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

static size_t count_arguments(Token *tokens) {
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

static char **build_snoop_argv(Token *tokens) {
    size_t argument_count = count_arguments(tokens);
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

static bool parse_pid(const char *text, pid_t *pid) {
    if(text == NULL || text[0] == '\0') {
        return false;
    }
    for(size_t i = 0; text[i] != '\0'; i++) {
        if(!isdigit((unsigned char)text[i])) {
            return false;
        }
    }
    errno = 0;
    char *end;
    unsigned long long value = strtoull(text, &end, 10);
    if(errno == ERANGE || *end != '\0' || value == 0 || value > INT_MAX) {
        return false;
    }
    *pid = (pid_t)value;
    return true;
}

static bool snoop_command_mode(Token *tokens) {
    char resolved_path[PATH_MAX];
    if(!resolve_snoop_command(tokens->next->value, resolved_path, sizeof(resolved_path))) {
        printf("snoop: command not found\n");
        return true;
    }
    char **argv = build_snoop_argv(tokens->next);
    if(argv == NULL) {
        return false;
    }
    pid_t child = fork();
    if(child == -1) {
        perror("fork");
        free(argv);
        return false;
    }
    if(child == 0) {
        if(ptrace(PTRACE_TRACEME, 0, NULL, NULL) == -1) {
            _exit(EXIT_FAILURE);
        }
        if(raise(SIGSTOP) != 0) {
            _exit(EXIT_FAILURE);
        }
        execve(resolved_path, argv, environ);
        _exit(EXIT_FAILURE);
    }
    int status;
    while(waitpid(child, &status, 0) == -1) {
        if(errno == EINTR) {
            continue;
        }
        free(argv);
        return false;
    }
    if(!WIFSTOPPED(status)) {
        free(argv);
        return false;
    }
    if(ptrace(PTRACE_SETOPTIONS, child, NULL, PTRACE_O_TRACESYSGOOD) == -1) {
        perror("ptrace");
        free(argv);
        return false;
    }
    if(ptrace(PTRACE_SYSCALL, child, NULL, NULL) == -1) {
        perror("ptrace");
        free(argv);
        return false;
    }
    while(waitpid(child, &status, 0) == -1) {
        if(errno == EINTR) {
            continue;
        }
        break;
    }
    if(WIFSTOPPED(status)) {
        ptrace(PTRACE_SYSCALL, child, NULL, NULL);
    }
    while(waitpid(child, &status, 0) == -1) {
        if(errno == EINTR) {
            continue;
        }
        break;
    }
    free(argv);
    return true;
}

static bool snoop_pid_mode(pid_t pid) {
    if(ptrace(PTRACE_ATTACH, pid, NULL, NULL) == -1) {
        printf("snoop: no such process\n");
        return true;
    }
    int status;
    while(waitpid(pid, &status, 0) == -1) {
        if(errno == EINTR) {
            continue;
        }
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return false;
    }
    if(!WIFSTOPPED(status)) {
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return false;
    }
    if(ptrace(PTRACE_SETOPTIONS, pid, NULL, PTRACE_O_TRACESYSGOOD) == -1) {
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return false;
    }
    if(ptrace(PTRACE_SYSCALL, pid, NULL, NULL) == -1) {
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return false;
    }
    while(waitpid(pid, &status, 0) == -1) {
        if(errno == EINTR) {
            continue;
        }
        break;
    }
    if(WIFSTOPPED(status)) {
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
    }
    return true;
}

bool execute_snoop(Token *tokens) {
    if(tokens == NULL || tokens->type != TOKEN_WORD || strcmp(tokens->value, "snoop") != 0) {
        return false;
    }
    Token *current = tokens->next;
    if(current == NULL) {
        printf("snoop: invalid syntax\n");
        return true;
    }
    if(strcmp(current->value, "-p") == 0) {
        current = current->next;
        if(current == NULL || current->type != TOKEN_WORD || current->next != NULL) {
            printf("snoop: invalid syntax\n");
            return true;
        }
        pid_t pid;
        if(!parse_pid(current->value, &pid)) {
            printf("snoop: no such process\n");
            return true;
        }
        return snoop_pid_mode(pid);
    }
    current = tokens->next;
    while(current != NULL) {
        if(current->type != TOKEN_WORD) {
            printf("snoop: invalid syntax\n");
            return true;
        }
        current = current->next;
    }
    return snoop_command_mode(tokens);
}