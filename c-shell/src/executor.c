#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/stat.h>
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
            written = snprintf(candidate, sizeof(candidate, )"%s/%s", current_directory, name);
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