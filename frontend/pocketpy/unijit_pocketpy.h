#ifndef UNIJIT_FRONTEND_POCKETPY_UNIJIT_POCKETPY_H
#define UNIJIT_FRONTEND_POCKETPY_UNIJIT_POCKETPY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool unijit_pocketpy_compile(const char *source);
int unijit_pocketpy_install(void);

#ifdef __cplusplus
}
#endif

#endif // UNIJIT_FRONTEND_POCKETPY_UNIJIT_POCKETPY_H
