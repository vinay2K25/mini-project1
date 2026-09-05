#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <stdbool.h>
#include "lexer.h"
bool execute_command(Token *tokens, bool background);
void initialise_executor(void);
void print_completed_background_jobs(void);
void print_activities(void);
bool has_stopped_jobs(void);
void shutdown_executor(void);

#endif