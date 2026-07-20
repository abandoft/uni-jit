#ifndef UNIJIT_FRONTEND_QUICKJS_UNIJIT_QUICKJS_H
#define UNIJIT_FRONTEND_QUICKJS_UNIJIT_QUICKJS_H

#include <quickjs.h>

#ifdef __cplusplus
extern "C" {
#endif

JSValue unijit_quickjs_compile(JSContext* context,
                               JSValueConst function_value);
int unijit_quickjs_install(JSContext* context);

#ifdef __cplusplus
}
#endif

#endif  // UNIJIT_FRONTEND_QUICKJS_UNIJIT_QUICKJS_H
