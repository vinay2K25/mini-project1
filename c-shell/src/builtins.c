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
#include "executor.h"
#include <ctype.h>
#include "snoop.h"

// PATH_MAX
// File to store the frecencies of the directories!
#define FRECENCY_FILE ".cshell_frecency"

// Dynamic buff to handle reading file in small chunks, specially during rev!
#define PEEK_BUFFER_SIZE 4096

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
        if(current->type == TOKEN_PIPE || current->type == TOKEN_SEMI || current->type == TOKEN_AMP) {
            break;
        }
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
        if(current->type == TOKEN_PIPE || current->type == TOKEN_SEMI || current->type == TOKEN_AMP) {
            break;
        }
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

// Func to count the num of non - empty lines in the file!
static bool count_nonempty_lines(int fd, size_t *count) {
    char buffer[PEEK_BUFFER_SIZE];
    size_t nonempty_lines = 0;
    bool line_has_content = false;
    while(true) {
        ssize_t bytes_read = read(fd, buffer, sizeof(buffer));
        if(bytes_read == 0) {
            break;
        }
        if(bytes_read < 0) {
            // EINTR indicates interrupted func call - prog is stopped mid-way thru exec by hardware/os call!
            if(errno == EINTR) {
                continue;
            }
            return false;
        }
        for(ssize_t i = 0; i < bytes_read; i++) {
            if(buffer[i] == '\n') {
                if(line_has_content) {
                    nonempty_lines++;
                }
                line_has_content = false;
            }
            else {
                line_has_content = true;
            }
        }
    }
    // Final line need not end with '\n'!
    if(line_has_content) {
        nonempty_lines++;
    }
    *count = nonempty_lines;
    return true;
} 

// Read thru file in fixed sized chunks and write them to stdout!
static bool peek_forward(int fd) {
    char buffer[PEEK_BUFFER_SIZE];
    while(true) {
        ssize_t bytes_read = read(fd, buffer, sizeof(buffer));
        if(bytes_read == 0) {
            return true;
        }
        if(bytes_read < 0) {
            if(errno == EINTR) {
                continue;
            }
            return false;
        }
        ssize_t total_written = 0;
        while(total_written < bytes_read) {
            ssize_t bytes_written = write(STDOUT_FILENO, buffer + total_written, bytes_read - total_written);
            if(bytes_written < 0) {
                if(errno == EINTR) {
                    continue;
                }
                return false;
            }
            total_written += bytes_written;
        }
    }
}

// Processing lines individually!
static bool peek_forward_numbered(int fd) {
    char buffer[PEEK_BUFFER_SIZE];
    PeekLine line;
    initialise_peek_line(&line);
    size_t line_number = 0;
    while(true) {
        ssize_t bytes_read = read(fd, buffer, sizeof(buffer));
        if(bytes_read == 0) {
            break;
        }
        if(bytes_read < 0) {
            if(errno == EINTR) {
                continue;
            }
            free_peek_line(&line);
            return false;
        }
        for(ssize_t i = 0; i < bytes_read; i++) {
            char character = buffer[i];
            if(character == '\n') {
                // Ignore empty lines completely!
                if(line.length > 0) {
                    line_number++;
                    // %zu is used to print the size_t var!
                    printf("%zu ", line_number);
                    fwrite(line.data, 1, line.length, stdout);
                    putchar('\n');
                }
                line.length = 0;
            }
            else {
                if(!append_peek_character(&line, character)) {
                    free_peek_line(&line);
                    return false;
                }
            }
        }
    }
    // Final line need not contain '\n'!
    if(line.length > 0) {
        line_number++;
        printf("%zu ", line_number);
        fwrite(line.data, 1, line.length, stdout);
        putchar('\n');
    }
    free_peek_line(&line);
    return true;
}

// Rev a regular file using lseek()!
static bool peek_reverse_regular(int fd, bool numbered) {
    // Determining how many non-empty lines exist when numbering is requested!
    size_t total_lines = 0;
    if(numbered) {
        // lseek() on err has ret val (off_t)-1!
        if(lseek(fd, 0, SEEK_SET) == (off_t)-1) {
            return false;
        }
        if(!count_nonempty_lines(fd, &total_lines)) {
            return false;
        }
    }
    // Moving to the EOF!
    off_t position = lseek(fd, 0, SEEK_END);
    if(position == (off_t)-1) {
        return false;
    }
    char buffer[PEEK_BUFFER_SIZE];
    PeekLine line;
    initialise_peek_line(&line);
    size_t printed_lines = 0;
    while(position > 0) {
        off_t chunk_size = position < (off_t)sizeof(buffer) ? position : (off_t)sizeof(buffer);
        position -= chunk_size;
        if(lseek(fd, position, SEEK_SET) == (off_t)-1) {
            free_peek_line(&line);
            return false;
        }
        ssize_t bytes_read = read(fd, buffer, (size_t)chunk_size);
        if(bytes_read < 0) {
            if(errno == EINTR) {
                // Retry the same chunk!
                position += chunk_size;
                continue;
            }
            free_peek_line(&line);
            return false;
        }
        // Scan the chunk backwards!
        for(ssize_t i = bytes_read - 1; i >= 0; i--) {
            char character = buffer[i];
            if(character == '\n') {
                // This indicates we've reached the beginning of the line!
                if(line.length > 0) {
                    reverse_peek_line(&line);
                    if(numbered) {
                        printf("%zu ", total_lines - printed_lines);
                    }
                    fwrite(line.data, 1, line.length, stdout);
                    putchar('\n');
                    printed_lines++;
                }
                line.length = 0;
            }
            else {
                if(!append_peek_character(&line, character)) {
                    free_peek_line(&line);
                    return false;
                }
            }
        }
    }
    // There's a final line at the beginning of the file, it may not be preceded by a '\n'!
    if(line.length > 0) {
        reverse_peek_line(&line);
        if(numbered) {
            printf("%zu ", total_lines - printed_lines);
        }
        fwrite(line.data, 1, line.length, stdout);
        putchar('\n');
    }
    free_peek_line(&line);
    return true;
}

// Revstdin/non - seekable input!
// Non-seekable input is similar to stdin by nature - you cannot jump to a specific pos in it!
static bool peek_reverse_stream(int fd, bool numbered) {
    PeekLine *lines = NULL;
    size_t line_count = 0;
    PeekLine current;
    initialise_peek_line(&current);
    char buffer[PEEK_BUFFER_SIZE];
    while(true) {
        ssize_t bytes_read = read(fd, buffer, sizeof(buffer));
        if(bytes_read == 0) {
            break;
        }
        if(bytes_read < 0) {
            if(errno == EINTR) {
                continue;
            }
            free_peek_line(&current);
            for(size_t i = 0; i < line_count; i++) {
                free_peek_line(&lines[i]);
            }
            free(lines);
            return false;
        }
        for(ssize_t i = 0; i < bytes_read; i++) {
            if(buffer[i] == '\n') {
                if(current.length > 0) {
                    PeekLine *new_lines = realloc(lines, (line_count + 1) * sizeof(PeekLine));
                    if(new_lines == NULL) {
                        free_peek_line(&current);
                        for(size_t j = 0; j < line_count; j++) {
                            free_peek_line(&lines[j]);
                        }
                        free(lines);
                        return false;
                    }
                    lines = new_lines;
                    lines[line_count] = current;
                    line_count++;
                    initialise_peek_line(&current);
                }
            }
            else {
                if(!append_peek_character(&current, buffer[i])) {
                    free_peek_line(&current);
                    for(size_t j = 0; j < line_count; j++) {
                        free_peek_line(&lines[j]);
                    }
                    free(lines);
                    return false;
                }
            }
        }
    }
    // Handling the final line present without a trailing '\n'!
    if(current.length > 0) {
        PeekLine *new_lines = realloc(lines, (line_count + 1) * sizeof(PeekLine));
        if(new_lines == NULL) {
            free_peek_line(&current);
            for(size_t i = 0; i < line_count; i++) {
                free_peek_line(&lines[i]);
            }
            free(lines);
            return false;
        }
        lines = new_lines;
        lines[line_count] = current;
        line_count++;
        initialise_peek_line(&current);
    }
    // The code block above has saved the lines into a buffer, and then we print them in rev from that buffer!
    // Printing the lines in rev!
    for(size_t i = line_count; i > 0; i--) {
        size_t index = i - 1;
        if(numbered) {
            printf("%zu ", index + 1);
        }
        fwrite(lines[index].data, 1, lines[index].length, stdout);
        putchar('\n');
    }
    for(size_t i = 0; i < line_count; i++) {
        free_peek_line(&lines[i]);        
    }
    free(lines);
    return true;
}

// Common file processor, decides whether it's a reg file, dir or non-seekable input!
static void peek_file(const char *filename, bool numbered, bool reverse) {
    int fd = open(filename, O_RDONLY);
    if(fd == -1) {
        printf("peek: no such file or directory\n");
        return;
    }
    struct stat information;
    // fstat() takes the attributes of the file/other i/o resources that the open fd points to, and places them in a buffer!
    // Common attri include file size, perms and modification times!
    if(fstat(fd, &information) == -1) {
        close(fd);
        printf("peek: no such file or directory\n");
        return;
    }

    // Checking if the file entered is actually a dir!
    if(S_ISDIR(information.st_mode)) {
        close(fd);
        printf("peek: is a directory\n");
        return;
    }

    bool success;
    if(!reverse) {
        if(numbered) {
            success = peek_forward_numbered(fd);
        }
        else {
            success = peek_forward(fd);
        }
    }
    else if(S_ISREG(information.st_mode)) {
        success = peek_reverse_regular(fd, numbered);
    }
    else {
        // Pipes, FIFOs etc. are non-seekable, so we save them to buffer and then process them!
        success = peek_reverse_stream(fd, numbered);
    }
    // Prevents unused var warning - we're using the -Werror flag in the Makefile!
    (void)success;
    close(fd);
}

// Handling stdin - peak and peak - indicate stdin!
static void peek_stdin(bool numbered, bool reverse) {
    if(!reverse) {
        if(numbered) {
            peek_forward_numbered(STDIN_FILENO);
        }
        else {
            peek_forward(STDIN_FILENO);
        }
        return;
    }
    peek_reverse_stream(STDIN_FILENO, numbered);
}

// Parsing the peek flags!
static bool parse_peek_flag(const char *argument, bool *numbered, bool *reverse) {
    if(argument[0] != '-' || argument[1] == '\0') {
        return false;
    }
    for(size_t i = 1; argument[i] != '\0'; i++) {
        if(argument[i] == 'n') {
            *numbered = true;
        }
        else if(argument[i] == 'r') {
            *reverse = true;
        }
        else {
            return false;
        }
    }
    return true;
}

static void execute_peek(Token *tokens) {
    bool numbered = false;
    bool reverse = false;
    size_t file_count = 0;
    Token *current = tokens->next;
    while(current != NULL) {
        if(current->type == TOKEN_PIPE || current->type == TOKEN_SEMI || current->type == TOKEN_AMP) {
            break;
        }
        if(current->type != TOKEN_WORD) {
            return;
        }
        // '-' by itself indicates stdin!
        if(strcmp(current->value, "-") == 0) {
            file_count++;
        }
        // Anything else beginning with '-' is a flag!
        else if(current->value[0] == '-') {
            if(!parse_peek_flag(current->value, &numbered, &reverse)) {
                printf("peek: invalid syntax\n");
                return;
            }
        }
        else {
            file_count++;
        }
        current = current->next;
    }
    // No filenames also indicate stdin!
    if(file_count == 0) {
        peek_stdin(numbered, reverse);
        return;
    }
    // Process every filename in the order supplied!
    current = tokens->next;
    while(current != NULL) {
        if(current->type == TOKEN_PIPE || current->type == TOKEN_SEMI || current->type == TOKEN_AMP) {
            break;
        }
        if(current->type != TOKEN_WORD) {
            break;
        }
        // Skip flags!
        if(current->value[0] == '-' && strcmp(current->value, "-") != 0) {
            current = current->next;
            continue;
        }
        // '-' indicates stdin!
        if(strcmp(current->value, "-") == 0) {
            peek_stdin(numbered, reverse);
        }
        else {
            peek_file(current->value, numbered, reverse);
        }
        current = current->next;
    }
}

// Helper func to determine if file is executable or not!
static bool is_executable_file(const char *path) {
    struct stat information;
    if(stat(path, &information) == -1) {
        return false;
    }
    // Targ file must be an executable!
    // A symbolic link to an exec reg file is also accepted as well! These refer to special types of files that point to other files!
    if(!S_ISREG(information.st_mode)) {
        return false;
    }
    return access(path, X_OK) == 0;
}

// Helper func to convert rel file path to an abs one!
static bool make_absolute_path(const char *directory, const char *filename, char *result, size_t size) {
    char current_directory[PATH_MAX];
    if(directory[0] == '\0') {
        // An empty path comp indicates the curr dir!
        if(getcwd(current_directory, sizeof(current_directory)) == NULL) {
            return false;
        }
        int written = snprintf(result, size, "%s/%s", current_directory, filename);
        return written >= 0 && (size_t)written < size;
    }
    // Already an abs path!
    if(directory[0] == '/') {
        int written = snprintf(result, size, "%s/%s", directory, filename);
        return written >= 0 && (size_t)written < size;
    }
    // Rel path comp!
    if(getcwd(current_directory, sizeof(current_directory)) == NULL) {
        return false;
    }
    int written = snprintf(result, size, "%s/%s/%s", current_directory, directory, filename);
    return written >= 0 && (size_t)written < size;
    // Using the realpath() func would collapse both /usr/bin/gcc and /bin/gcc into a single abs path /usr/bin/gcc, and we don't want this!
}

// Lookup for one cmd!
static void locate_one(const char *filename) {
    bool found = false;
    // We first search the cwd, then other dir!
    char current_directory[PATH_MAX];
    if(getcwd(current_directory, sizeof(current_directory)) != NULL) {
        char candidate[PATH_MAX];
        int written = snprintf(candidate, sizeof(candidate), "%s/%s", current_directory, filename);
        if(written >= 0 && (size_t)written < sizeof(candidate) && is_executable_file(candidate)) {
            printf("%s\n", candidate);
            found = true;
        }
    }
    // We now search every dir listed in PATH, in the same order as the appear!
    // Env var are key-val pairs stored by the os containing system wide conf data!
    // getenv() searches for that key-value pair and ret a pointer to the val!
    const char *path_environment = getenv("PATH");
    if(path_environment != NULL) {
        char *path_copy = malloc(strlen(path_environment) + 1);

        if(path_copy != NULL) {
            strcpy(path_copy, path_environment);
            char *directory = path_copy;
            while(true) {
                // Returns a pointer to the first occ of a char in the str!
                char *separator = strchr(directory, ':');
                if(separator != NULL) {
                    *separator = '\0';
                }
                char candidate[PATH_MAX];
                if(make_absolute_path(directory, filename, candidate, sizeof(candidate))) {
                    if(is_executable_file(candidate)) {
                        printf("%s\n", candidate);
                        found = true;
                    }
                }
                if(separator == NULL) {
                    break;
                }
                directory = separator + 1;
            }
        }
        free(path_copy);
    }
    // No executable was found anywhere!
    if(!found) {
        printf("locate: command not found (%s)\n", filename);
    }
}

static void execute_locate(Token *tokens) {
    Token *current = tokens->next;
    while(current != NULL) {
        if(current->type == TOKEN_PIPE || current->type == TOKEN_SEMI || current->type == TOKEN_AMP) {
            break;
        }
        if(current->type != TOKEN_WORD) {
            return;
        }
        locate_one(current->value);
        current = current->next;
    }
}

static void execute_ping(Token *tokens) {
    Token *current = tokens->next;
    if(current == NULL || current->type != TOKEN_WORD) {
        printf("ping: invalid syntax\n");
        return;
    }
    const char *target = current->value;
    current = current->next;
    if(current == NULL || current->type != TOKEN_WORD) {
        printf("ping: invalid syntax\n");
        return;
    }
    const char *signal_string = current->value;
    current = current->next;
    if(current != NULL) {
        printf("ping: invalid syntax\n");
        return;
    }
    if(signal_string[0] == '\0') {
        printf("ping: invalid syntax\n");
        return;
    }
    for(size_t i = 0; signal_string[i] != '\0'; i++) {
        if(!isdigit((unsigned char)signal_string[i])) {
            printf("ping: invalid syntax\n");
            return;
        }
    }
    errno = 0;
    char *end;
    unsigned long long signal_value = strtoull(signal_string, &end, 10);
    if(errno == ERANGE || *end != '\0' || signal_value > ULLONG_MAX) {
        printf("ping: invalid syntax\n");
        return;
    }
    int actual_signal = (int)(signal_value % 64);
    if(!ping_target(target, actual_signal)) {
        printf("ping: no such process found\n");
        return;
    }
    printf("Sent signal %llu to %s\n", signal_value, target);
}

// Read the target of a symbolic link and store it in result!
static bool read_proc_link(const char *link_path, char *result, size_t size) {
    ssize_t length = readlink(link_path, result, size - 1);
    if(length == -1) {
        return false;
    }
    result[length] = '\0';
    return true;
}

// Convert a file's stat information into the TYPE expected by spy!
static const char *spy_file_type(const char *path) {
    struct stat information;
    if(stat(path, &information) == -1) {
        return "UNKNOWN";
    }
    if(S_ISREG(information.st_mode)) {
        return "REG";
    }
    if(S_ISDIR(information.st_mode)) {
        return "DIR";
    }
    if(S_ISCHR(information.st_mode)) {
        return "CHR";
    }
    if(S_ISBLK(information.st_mode)) {
        return "BLK";
    }
    if(S_ISFIFO(information.st_mode)) {
        return "FIFO";
    }
    if(S_ISSOCK(information.st_mode)) {
        return "SOCK";
    }
    return "UNKWOWN";
}

// Print one special /proc entry such as cwd or txt!
static void spy_print_special(pid_t pid, const char *fd_name, const char *proc_name) {
    char link_path[PATH_MAX];
    char target[PATH_MAX];
    int written = snprintf(link_path, sizeof(link_path), "/proc/%ld/%s", (long)pid, proc_name);
    if(written < 0 || (size_t)written >= sizeof(link_path)) {
        return;
    }
    if(!read_proc_link(link_path, target, sizeof(target))) {
        return;
    }
    // cwd and txt are both links to file-system objects!
    const char *type = spy_file_type(target);
    printf("%-6ld %-5s %-7s %s\n", (long)pid, fd_name, type, target);
}

// Check whether a memory mapped file has already been printed!
static bool spy_mem_seen(char **paths, size_t count, const char *path) {
    for(size_t i = 0; i < count; i++) {
        if(strcmp(paths[i], path) == 0) {
            return true;
        }
    }
    return false;
}

// Print unique memory-mapped files from /proc/<pid>/maps!
static void spy_print_mem(pid_t pid) {
    char maps_path[PATH_MAX];
    int written = snprintf(maps_path, sizeof(maps_path), "/proc/%ld/maps", (long)pid);
    if(written < 0 || (size_t)written >= sizeof(maps_path)) {
        return;
    }
    FILE *file = fopen(maps_path, "r");
    if(file == NULL) {
        return;
    }
    char **seen_paths = NULL;
    size_t seen_count = 0;
    char line[PATH_MAX + 512];
    while(fgets(line, sizeof(line), file) != NULL) {
        char *pathname = strchr(line, '/');
        if(pathname == NULL) {
            continue;
        }
        pathname[strcspn(pathname, "\n")] = '\0';
        if(pathname[0] == '\0') {
            continue;
        }
        if(spy_mem_seen(seen_paths, seen_count, pathname)) {
            continue;
        }
        char *saved_path = malloc(strlen(pathname) + 1);
        if(saved_path == NULL) {
            break;
        }
        strcpy(saved_path, pathname);
        char **new_paths = realloc(seen_paths, (seen_count + 1) * sizeof(char *));
        if(new_paths == NULL) {
            free(saved_path);
            return;
        }
        seen_paths = new_paths;
        seen_paths[seen_count] = saved_path;
        seen_count++;
        const char *type = spy_file_type(pathname);
        printf("%-6ld %-5s %-7s %s\n", (long)pid, "mem", type, pathname);
    }
    for(size_t i = 0; i < seen_count; i++) {
        free(seen_paths[i]);
    }
    free(seen_paths);
    fclose(file);
}

// Print all numeric file desc from /proc/<pid>/fd!
static void spy_print_fds(pid_t pid) {
    char fd_directory[PATH_MAX];
    int written = snprintf(fd_directory, sizeof(fd_directory), "/proc/%ld/fd", (long)pid);
    if(written < 0 || (size_t)written >= sizeof(fd_directory)) {
        return;
    }
    DIR *directory = opendir(fd_directory);
    if(directory == NULL) {
        return;
    }
    int directory_fd = dirfd(directory);
    struct dirent *entry;
    while((entry = readdir(directory)) != NULL) {
        // Only numeric directory entries represent file desc!
        if(entry->d_name[0] == '\0') {
            continue;
        }
        bool numeric = true;
        for(size_t i = 0; entry->d_name[i] != '\0'; i++) {
            if(!isdigit((unsigned char)entry->d_name[i])) {
                numeric = false;
                break;
            }
        }
        if(!numeric) {
            continue;
        }
        errno = 0;
        char *end;
        long fd_number = strtol(entry->d_name, &end, 10);
        if(errno == ERANGE || *end != '\0' || fd_number < 0) {
            continue;
        }
        // Ignore the directory fd created by opendir itself!
        if(fd_number == directory_fd) {
            continue;
        }
        char link_path[PATH_MAX];
        char target[PATH_MAX];
        written = snprintf(link_path, sizeof(link_path), "%s/%s", fd_directory, entry->d_name);
        if(written < 0 || (size_t)written >= sizeof(link_path)) {
            continue;
        }
        if(!read_proc_link(link_path, target, sizeof(target))) {
            // The fd may have disappeared while spy was examining it!
            continue;
        }
        const char *type = spy_file_type(link_path);
        printf("%-6ld %-5s %-7s %s\n", (long)pid, entry->d_name, type, target);
    }
    closedir(directory);
}

// Execute the spy built-in!
static void execute_spy(Token *tokens) {
    Token *current = tokens->next;
    if(current != NULL && current->next != NULL) {
        printf("spy: invalid syntax\n");
        return;
    }
    pid_t pid;
    if(current == NULL) {
        pid = getpid();
    }
    else {
        if(current->type != TOKEN_WORD || current->value[0] == '\0') {
            printf("spy: no such process\n");
            return;
        }
        for(size_t i = 0; current->value[i] != '\0'; i++) {
            if(!isdigit((unsigned char)current->value[i])) {
                printf("spy: no such process\n");
                return;
            }
        }
        errno = 0;
        char *end;
        long pid_value = strtol(current->value, &end, 10);
        if(errno == ERANGE || *end != '\0' || pid_value <= 0 || pid_value > INT_MAX) {
            printf("spy: no such process\n");
            return;
        }
        pid = (pid_t)pid_value;
    }
    char process_directory[PATH_MAX];
    int written = snprintf(process_directory, sizeof(process_directory), "/proc/%ld", (long)pid);
    if(written < 0 || (size_t)written >= sizeof(process_directory)) {
        printf("spy: no such process\n");
        return;
    }
    struct stat information;
    if(stat(process_directory, &information) == -1 || !S_ISDIR(information.st_mode)) {
        printf("spy: no such process\n");
        return;
    }
    printf("%-6s %-5s %-7s %s\n", "PID", "FD", "TYPE", "PATH");
    spy_print_special(pid, "cwd", "cwd");
    spy_print_special(pid, "txt", "exe");
    spy_print_mem(pid);
    spy_print_fds(pid);
}

// Func to check if the cmd is a built-in cmd or not!
bool is_builtin_command(Token *tokens) {
    if(tokens == NULL || tokens->type != TOKEN_WORD) {        
        return false;
    }
    if(strcmp(tokens->value, "hop") == 0) {
        return true;
    }
    if(strcmp(tokens->value, "reveal") == 0) {
        return true;
    }
    if(strcmp(tokens->value, "peek") == 0) {
        return true;
    }
    if(strcmp(tokens->value, "locate") == 0) {
        return true;
    }
    if(strcmp(tokens->value, "activities") == 0) {
        return true;
    }
    if(strcmp(tokens->value, "resume") == 0) {
        return true;
    }
    if(strcmp(tokens->value, "ping") == 0) {
        return true;
    }
    if(strcmp(tokens->value, "spy") == 0) {
        return true;
    }
    return false;
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
    if(strcmp(tokens->value, "peek") == 0) {
        execute_peek(tokens);
        return true;
    }
    if(strcmp(tokens->value, "locate") == 0) {
        execute_locate(tokens);
        return true;
    }
    if(strcmp(tokens->value, "activities") == 0) {
        print_activities();
        return true;
    }
    if(strcmp(tokens->value, "resume") == 0) {
        resume_job(tokens);
        return true;
    }
    if(strcmp(tokens->value, "ping") == 0) {
        execute_ping(tokens);
        return true;
    }
    if(strcmp(tokens->value, "spy") == 0) {
        execute_spy(tokens);
        return true;
    }
    return false;
}