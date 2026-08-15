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
        // Informing the user of a lexical error, that is, an error in their command-syntax!
        if(tokens == NULL) {
            printf("cshell: invalid syntax\n");
            continue;
        }
        print_tokens(tokens);
        free_tokens(tokens);
    }
    return 0;
}