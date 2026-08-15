#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <limits.h>
#include <pwd.h>
#include <string.h>
#include "prompt.h"
#include "lexer.h"

int main()
{
    initialise_shell();
    char input[4096];
    while(true) {
        print_prompt();
        if(fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }
        Token *tokens = lex(input);
        print_tokens(tokens);
        free_tokens(tokens);
    }
    return 0;
}