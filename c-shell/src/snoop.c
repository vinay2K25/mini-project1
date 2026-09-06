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