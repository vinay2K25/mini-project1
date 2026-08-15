#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <limits.h>
#include <pwd.h>
#include <string.h>
#include "prompt.h"

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