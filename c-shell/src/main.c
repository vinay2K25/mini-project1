#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <limits.h>
#include <pwd.h>
#include <string.h>
#include <errno.h>
#include "prompt.h"
#include "lexer.h"
#include "parser.h"
#include "builtins.h"
#include "executor.h"
#include "sequential.h"
int main()
{
    initialise_shell();
    initialise_builtins();
    initialise_executor();
    char input[4096];
    bool eof_seen = false;
    while(true) {
        print_completed_background_jobs();
        print_prompt();
        if(fgets(input, sizeof(input), stdin) == NULL) {
            if(has_stopped_jobs()) {
                if(!eof_seen) {
                    printf("cshell: there are stopped jobs\n");
                    eof_seen = true;
                    clearerr(stdin);
                    continue;
                }
                shutdown_executor();
                printf("\n");
                break;
            }
            shutdown_executor();
            printf("\n");
            break;
        }
        eof_seen = false;
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
        if(!execute_builtin(tokens)) {
            execute_sequential(tokens);
        }
        free_tokens(tokens);
    }
    return 0;
}