#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "sequential.h"
#include "executor.h"
#include "builtins.h"
static Token *find_command_end(Token *start) {
    Token *current = start;
    while(current != NULL) {
        if(current->type == TOKEN_SEMI || current->type == TOKEN_AMP) {
            break;
        }
        current = current->next;
    }
    return current;
}