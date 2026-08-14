#include <stdio.h>
#include <stdlib.h>
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
    printf("<%s@%s:%s> ", pw->pw_name, hostname, current_dir);
}

int main()
{
    initialise_shell();
    print_prompt();
    return 0;
}