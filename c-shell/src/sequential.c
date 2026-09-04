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

static bool execute_one_command(Token *start, Token *end) {
    if(start == NULL) {
        return true;
    }
    // We temporarily terminate the following command groups!
    // As an example, consider echo Hello! ; echo World!
    // Here, we pause the execution of the echo World! command!
    Token *saved_next = NULL;
    if(end != NULL) {
        saved_next = end->next;
        end->next = NULL;
    }
    // this variable helps us decide if we need to execute the commands following the current command!
    bool success;
    if(execute_builtin(start)) {
        success = true;
    }
    else {
        bool background = (end != NULL && end->type == TOKEN_AMP);
        success = execute_command(start, background);
    }
    if(end != NULL) {
        end->next = saved_next;
    }    
    return success;
}

bool execute_sequential(Token *tokens) {
    Token *current = tokens;
    while(current != NULL) {
        Token *end = find_command_end(current);
        // Execute everything before ';' or '&'!
        if(!execute_one_command(current, end)) {
            return false;
        }
        // If no other separator is detected, it marks the end of execution!
        if(end == NULL) {
            break;
        }
        // '&' will be handled in D-2!
        if(end->type == TOKEN_AMP) {
            current = end->next;
            continue;
        }
        // Skip the incoming ';' and start executing the next command!
        current = end->next;
    }
    return true;
}