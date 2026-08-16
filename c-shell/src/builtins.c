#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include "builtins.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>

// PATH_MAX
// File to store the frecencies of the directories!
#define FRECENCY_FILE ".cshell_frecency"

// Dynamic buff to handle reading file in small chunks, specially during rev!
#define PPEK_BUFFER_SIZE 4096

typedef struct FrecencyEntry {
    char path[PATH_MAX];
    long long frequency;
    long long last_visit;
    struct FrecencyEntry *next;
} FrecencyEntry;

typedef struct RevealEntry {
    char *name;
    bool is_directory;
} RevealEntry;

typedef struct PeekLine {
    char *data;
    size_t length;
    size_t capacity;
} PeekLine;

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

// Path resolver for reveal - this will convert a rel file path into an abs file path!
static bool resolve_reveal_path(const char *argument, char *result, size_t size) {
    if(strcmp(argument, "~") == 0) {
        snprintf(result, size, "%s", home_directory);
        return true;
    }
    if(strcmp(argument, ".") == 0) {
        if(getcwd(result, size) == NULL) {
            return false;
        }
        return true;
    }
    if(strcmp(argument, "..") == 0) {
        char current[PATH_MAX];
        if(getcwd(current, sizeof(current)) == NULL) {
            return false;
        }
        if(realpath("..", result) == NULL) {
            return false;
        }
        return true;
    }
    if(strcmp(argument, "-") == 0) {
        if(!has_previous_directory) {
            return false;
        }
        snprintf(result, size, "%s", previous_directory);
        return true;
    }
    // For ordinary paths, resolve rel to cwd or accept an abs path!
    if(realpath(argument, result) == NULL) {
        return false;
    }
    return true;
}

static bool reveal_is_directory(const char *path) {
    struct stat information;
    if(stat(path, &information) == -1) {
        return false;
    }
    return S_ISDIR(information.st_mode);
}

// Sorting revealed entries!
static int compare_reveal_entries(const void *first, const void *second) {
    const RevealEntry *a = first;
    const RevealEntry *b = second;
    return (strcmp(a->name, b->name));
}

static RevealEntry *read_reveal_entries(const char *directory, bool show_hidden, size_t *count) {
    DIR *dir = opendir(directory);
    if(dir == NULL) {
        return NULL;
    }
    RevealEntry *entries = NULL;
    size_t entry_count = 0;
    struct dirent *entry;
    while((entry = readdir(dir)) != NULL) {
        // "." and ".." are never displayed as dir entries!
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        // Hidden files are omitted unless -a flag is detected!
        if(!show_hidden && entry->d_name[0] == '.') {
            continue;
        }
        RevealEntry *new_entries = realloc(entries, (entry_count + 1) * sizeof(RevealEntry));
        if(new_entries == NULL) {
            free(entries);
            closedir(dir);
            return NULL;
        }
        entries = new_entries;
        entries[entry_count].name = malloc(strlen(entry->d_name) + 1);
        if(entries[entry_count].name == NULL) {
            for(size_t i = 0; i < entry_count; i++) {
                free(entries[i].name);
            }
            free(entries);
            closedir(dir);
            return NULL;
        }
        strcpy(entries[entry_count].name, entry->d_name);
        char full_path[PATH_MAX];
        int written = snprintf(full_path, sizeof(full_path), "%s/%s", directory, entry->d_name);
        if(written < 0 || (size_t)written >= sizeof(full_path)) {
            entries[entry_count].is_directory = false;
        }
        else {
            entries[entry_count].is_directory = reveal_is_directory(full_path);
        }
        entry_count++;
    }
    closedir(dir);
    qsort(entries, entry_count, sizeof(RevealEntry), compare_reveal_entries);
    *count = entry_count;
    return entries;
}

// Clean-up helper!
static void free_reveal_entries(RevealEntry *entries, size_t count) {
    for(size_t i = 0; i < count; i++) {
        free(entries[i].name);
    }
    free(entries);
}

// Recursive print func!
static void reveal_directory(const char *directory, bool show_hidden, bool recursive, const char *display_prefix) {
    size_t count = 0;
    RevealEntry *entries = read_reveal_entries(directory, show_hidden, &count);
    if(entries == NULL) {
        return;
    }
    for(size_t i = 0; i < count; i++) {
        char full_path[PATH_MAX];
        int written = snprintf(full_path, sizeof(full_path), "%s/%s", directory, entries[i].name);
        if(written < 0 || (size_t)written >= sizeof(full_path)) {
            continue;
        }
        // Constructing the path shown to user - at top lvl direct files, followed by nested dir!
        char display_path[PATH_MAX];
        if(display_prefix == NULL || display_prefix[0] == '\0') {
            snprintf(display_path, sizeof(display_path), "%s", entries[i].name);
        }
        else {
            snprintf(display_path, sizeof(display_path), "%s/%s", display_prefix, entries[i].name);
        }
        if(entries[i].is_directory) {
            printf("%s/\n", display_path);    
        }
        else {
            printf("%s\n", display_path);
        }
        // -t flag indicates to recursively reveal sub-dir!
        if(recursive && entries[i].is_directory) {
            char child_prefix[PATH_MAX];
            snprintf(child_prefix, sizeof(child_prefix), "%s", display_path);
            reveal_directory(full_path, show_hidden, recursive, child_prefix);
        }
    }
    free_reveal_entries(entries, count);
}

// Parsing the reveal flags!
static bool parse_reveal_flag(const char *argument, bool *show_hidden, bool *recursive) {
    // No flags were detected!
    if(argument[0] != '-' || argument[1] == '\0') {
        return false;
    }
    for(size_t i = 1; argument[i] != 0; i++) {
        // Show all files and dir, including hidden ones!
        if(argument[i] == 'a') {
            *show_hidden = true;
        }
        // Recursively show all contents!
        else if(argument[i] == 't') {
            *recursive = true;
        }
        else {
            return false;
        }
    }
    return true;
}

static void execute_reveal(Token *tokens) {
    bool show_hidden = false;
    bool recursive = false;
    char *target = NULL;
    Token *current = tokens->next;
    while(current != NULL) {
        if(current->type != TOKEN_WORD) {
            return;
        }
        // Anything beginning with '-' is considered to be a flag!
        if(current->value[0] == '-') {
            if(!parse_reveal_flag(current->value, &show_hidden, &recursive)) {
                printf("reveal: invalid syntax\n");
                return;
            }
        }
        else {
            // Reveal cmd accepts at most one dir arg!
            if(target != NULL) {
                printf("reveal: invalid syntax\n");
                return;
            }
            target = current->value;
        }
        current = current->next;
    }
    // No targ indicates cwd!
    char resolved_path[PATH_MAX];
    if(target == NULL) {
        if(getcwd(resolved_path, sizeof(resolved_path)) == NULL) {
            printf("reveal: no such directory\n");
            return;
        }
    }
    else {
        if(!resolve_reveal_path(target, resolved_path, sizeof(resolved_path))) {
            printf("reveal: no such directory\n");
            return;
        }
    }
    // The resolved targ must be an actual dir!
    if(!reveal_is_directory(resolved_path)) {
        printf("reveal: no such directory\n");
        return;
    }
    reveal_directory(resolved_path, show_hidden, recursive, NULL);
}

static void initialise_peek_line(PeekLine *line) {
    line->data = NULL;
    line->length = 0;
    line->capacity = 0;
}

// Append a char to the curr rev line!
static bool append_peek_character(PeekLine *line, char character) {
    if(line->length + 1 >= line->capacity) {
        size_t new_capacity = line->capacity == 0 ? 64 : line->capacity * 2;
        char *new_data = realloc(line->data, new_capacity);
        if(new_data == NULL) {
            return false;
        }
        line->data = new_data;
        line->capacity = new_capacity;
    }
    line->data[line->length++] = character;
    return true;
}

static void free_peek_line(PeekLine *line) {
    free(line->data);
    line->data = NULL;
    line->length = 0;
    line->capacity = 0;
}

// Rev the curr line!
static void reverse_peek_line(PeekLine *line) {
    if(line->length == 0) {
        return;
    }
    size_t left = 0;
    size_t right = line->length - 1;
    while(left < right) {
        char temporary = line->data[left];
        line->data[left] = line->data[right];
        line->data[right] = temporary;
        left++;
        right--;
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
    if(strcmp(tokens->value, "reveal") == 0) {
        execute_reveal(tokens);
        return true;
    }
    return false;
}