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
        int matched = sscanf(line, "%lld %lld %[^\n]", &entry->frequency, &entry->last_visit, entry->path);
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
// Free-ing up the frecency linked-list!
static void free_frecency(FrecencyEntry *head) {
    while(head != NULL) {
        FrecencyEntry *next = head->next;
        free(head);
        head = next;
    }
}
// Record-ing details of a successful visit to a path!
static void record_visit(const char *path) {
    FrecencyEntry *head = load_frecency();
    FrecencyEntry *entry = head;
    while(entry != NULL) {
        if(strcmp(entry->path, path) == 0) {
            break;
        }
        entry = entry->next;
    }
    if(entry == NULL) {
        entry = malloc(sizeof(FrecencyEntry));
        if(entry == NULL) {
            free_frecency(head);
            exit(EXIT_FAILURE);
        }
        snprintf(entry->path, sizeof(entry->path), "%s", path);
        entry->frequency = 0;
        entry->last_visit = 0;
        entry->next = head;
        head = entry;
    }
    entry->frequency++;
    visit_counter++;
    entry->last_visit = visit_counter;
    save_frecency(head);
    free_frecency(head);
}
// Determining whether a path exists and is a directory!
static bool is_directory(const char *path) {
    struct stat information;
    if(stat(path, &information) == -1) {
        return false;
    }
    return S_ISDIR(information.st_mode);
}
// Perform-ing dir change and updating all states associated with hop!
static bool change_directory(const char *path) {
    char old_directory[PATH_MAX];
    if(getcwd(old_directory, sizeof(old_directory)) == NULL) {
        return false;
    }
    if(chdir(path) == -1) {
        return false;
    }
    char new_directory[PATH_MAX];
    // We're already in the new dir, keep the shell usable!
    if(getcwd(new_directory, sizeof(new_directory)) == NULL) {
        return true;
    }
    snprintf(previous_directory, sizeof(previous_directory), "%s", old_directory);
    has_previous_directory = true;
    record_visit(new_directory);
    return true;
}
// Return best frecency match for a name!
static bool frecency_lookup(const char *name, char *result, size_t size) {
    FrecencyEntry *head = load_frecency();
    FrecencyEntry *best = NULL;
    long long best_score = -1;
    for(FrecencyEntry *entry = head; entry != NULL; entry = entry->next) {
        // Requested name must occur some-where when traversing the linked-list!
        if(strstr(entry->path, name) == NULL) {
            continue;
        }
        // The dir may have been deleted since it was recorded!
        if(!is_directory(entry->path)) {
            continue;
        }
        long long score = entry->frequency * 1000000LL + entry->last_visit;
        if(best == NULL || score > best_score) {
            best = entry;
            best_score = score;
        }
    }
    if(best == NULL) {
        free_frecency(head);
        return false;
    }
    snprintf(result, size, "%s", best->path);
    free_frecency(head);
    return true;
}
// Process one hop arg!
static void hop_one(const char *argument) {
    // Change cwd to shell's home dir!
    if(strcmp(argument, "~") == 0) {
        if(!change_directory(home_directory)) {
            printf("hop: no such directory\n");
        }
        return;
    }
    // Do nothing, stay in cwd!
    if(strcmp(argument, ".") == 0) {
        return;
    }
    // Move to the parent dir!
    // The chdir("..") cmd leaves us at root in-case we're already at root dir, this is desirable!
    if(strcmp(argument, "..") == 0) {
        if(!change_directory("..")) {
            printf("hop: no such directory\n");
        }
        return;
    }
    // Move to the prev cwd!
    if(strcmp(argument, "-") == 0) {
        if(!has_previous_directory) {
            return;
        }
        if(!change_directory(previous_directory)) {
            printf("hop: no such directory\n");
        }
        return;
    }
    // Interpret the arg directly rel to cwd or as an abs path!
    if(change_directory(argument)) {
        return;
    }
    // Dir resolution failed, fall back to frecency!
    char match[PATH_MAX];
    if(frecency_lookup(argument, match, sizeof(match))) {
        if(change_directory(match)) {
            return;
        }
    }
    printf("hop: no such directory\n");
}
// Executing the hop built-in!
static void execute_hop(Token *tokens) {
    Token *current = tokens->next;
    // No arg after hop indicates hop home!
    if(current == NULL) {
        hop_one("~");
        return;
    }
    while(current != NULL) {
        // Every token following hop must be TOKEN_WORD!
        // Only process TOKEN_WORD belong-ing to the cmd!
        if(current->type != TOKEN_WORD) {
            break;
        }
        hop_one(current->value);
        current = current->next;
    }
}
// Determine whether curr cmd is a built-in or not - ret true if so, else false if it needs to be handled in some other case!
bool execute_builtin(Token *tokens) {
    if(tokens == NULL || tokens->type != TOKEN_WORD) {
        return false;
    }
    if(strcmp(tokens->value, "hop") == 0) {
        execute_hop(tokens);
        return true;
    }
    return false;
}