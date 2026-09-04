#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <stdbool.h>
#include "lexer.h"
bool execute_command(Token *tokens, bool background);
void initialise_executor(void);

#endif