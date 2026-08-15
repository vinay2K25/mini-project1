#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include "builtins.h"

// File to store the frecencies of the directories!
#define FRECENCY_FILE ".cshell_frecency"

typedef struct FrecencyEntry {
    char path[PATH_MAX];
    long long frequency;
    long long last_visit;
    struct FrecencyEntry *next;
} FrecencyEntry;

static char home_directory[PATH_MAX];
static char previous_directory[PATH_MAX];
static bool has_previous_directory = false;
static long long visit_counter = 0;

// Return the path of the persistent frecency data-base!
static bool get_frecency_file(char *path, size_t size) {
    const char *home = getenv("HOME");
    if(home == NULL) {
        return false;
    }
    int written = snprintf(path, size, "%s/%s", home, FRECENCY_FILE);
    return written >= 0 && (size_t)written < size;
}

// Dir where shell is starter is considered to be the shell's home dir, not the user's actual home dir!
void initialise_builtins() {
    if(getcwd(home_directory, sizeof(home_directory)) == NULL) {
        perror("getcwd");
        exit(EXIT_FAILURE);
    }
    previous_directory[0] = '\0';
    has_previous_directory = false;
}

