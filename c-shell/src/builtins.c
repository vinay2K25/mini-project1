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

// Loading the persistent frecency data-base!
static FrecencyEntry *load_frecency() {
    char filename[PATH_MAX];
    if(!get_frecency_file(filename, sizeof(filename))) {
        return NULL;
    }
    FILE *file = fopen(filename, "r");
    // When running the shell for the first time, or when user deletes the file, it will not exist!
    if(file == NULL) {
        return NULL;
    }
    FrecencyEntry *head = NULL;
    FrecencyEntry *tail = NULL;
    char line[PATH_MAX + 64];    
    while(fgets(line, sizeof(line), file) != NULL) {
        FrecencyEntry *entry = malloc(sizeof(FrecencyEntry));
        if(entry == NULL) {
            fclose(file);
            exit(EXIT_FAILURE);
        }
        memset(entry, 0, sizeof(FrecencyEntry));
        int matched = sscanf(line, "&lld %lld %[^\n]", &entry->frequency, &entry->last_visit, entry->path);
        if(matched != 3 || entry->path[0] == '\0') {
            free(entry);
            continue;
        }
        if(entry->last_visit > visit_counter) {
            visit_counter = entry->last_visit;
        }
        entry->next = NULL;
        if(head == NULL) {
            head = entry;
            tail = entry;
        }
        else {
            tail->next = entry;
            tail = entry;
        }
        fclose(file);
        return head;
    }
    // Save the entire frecency data-base!
    static void save_frecency(FrecencyEntry *head) {
        char filename[PATH_MAX];
        if(!get_frecency_file(filename, sizeof(filename))) {
            return;
        }
        FILE *file = fopen(filename, "w");
        if(file == NULL) {
            return;
        }
        for(FrecencyEntry *entry = head; entry != NULL; entry = entry->next) {
            fprintf(file, "%lld %lld %s\n", entry->frequency, entry->last_visit, entry->path);
        }
        fclose(file);
    }
    
}