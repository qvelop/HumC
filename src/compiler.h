#ifndef HUMC_COMPILER_H
#define HUMC_COMPILER_H

#include <stddef.h>
#define HUMC_VERSION "1.0.0"

char *humc_compile(const char *source, char **error_message);

// Syntax-checks a HumC source file by compiling it in memory. 
int humc_check(const char *source, char **error_message);

char *humc_format(const char *source);
#endif
