#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <limits.h>
#include <pwd.h>
#include <string.h>
static char shell_home[PATH_MAX];

void initialise_shell() {
    if(getcwd(shell_home, sizeof(shell_home)) == NULL) {
        perror("getcwd");
        exit(EXIT_FAILURE);
    }
}

void print_prompt() {
    char current_dir[PATH_MAX];
    char hostname[HOST_NAME_MAX];
    struct passwd *pw = getpwuid(getuid());
    if(pw == NULL) {
        perror("getpwuid");
        exit(EXIT_FAILURE);
    }
    if(gethostname(hostname, sizeof(hostname)) == -1) {
        perror("gethostname");
        exit(EXIT_FAILURE);
    }
    if(getcwd(current_dir, sizeof(current_dir)) == NULL) {
        perror("getcwd");
        exit(EXIT_FAILURE);
    }
    // Handling the two cases - when the home directory as an ancestor of the pwd, and when it's not an ancestor of the pwd!
    if(strcmp(current_dir, shell_home) == 0) {
        printf("<%s@%s:~> ", pw->pw_name, hostname);
        return;
    }
    if(strncmp(current_dir, shell_home, strlen(shell_home)) == 0 && current_dir[strlen(shell_home)] == '/') {
        printf("<%s@%s:~%s> ", pw->pw_name, hostname, current_dir + strlen(shell_home));
        return;
    }
    printf("<%s@%s:%s> ", pw->pw_name, hostname, current_dir);
}

int main()
{
    initialise_shell();
    char input[4096];
    while(true) {
        print_prompt();
        if(fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }
    }
    return 0;
}