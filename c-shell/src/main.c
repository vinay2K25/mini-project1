#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <limits.h>
#include <pwd.h>
#include <string.h>
#include "prompt.h"
#include "lexer.h"
#include "parser.h"
#include "builtins.h"
int main()
{
    initialise_shell();
    initialise_builtins();
    char input[4096];
    while(true) {
        print_prompt();
        if(fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }
        bool lex_error;
        Token *tokens = lex(input, &lex_error);
        // Informing the user of a lexical error, that is, an error in their command-syntax!
        if(lex_error) {
            printf("cshell: invalid syntax\n");
            continue;
        }
        bool parse_result = parse(tokens);
        if(!parse_result) {
            printf("cshell: invalid syntax\n");
            free_tokens(tokens);
            continue;
        }
        // De-bugging func!
        // printf("parse: %s\n", parse_result ? "valid" : "invalid");
        // print_tokens(tokens);

        execute_builtin(tokens);
        free_tokens(tokens);
    }
    return 0;
}