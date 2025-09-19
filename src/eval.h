#ifndef _EVAL_H_
#define _EVAL_H_

#include "scripting_engine.h"

void evalInit(void);

void *evalActiveDefragScript(void *ptr);

/* udf helper */
sds genLuaLibInfoString(sds info);
sds genUdfInfoString(sds info);
int luaLibInit(const char *lib_path);
int luaUdfInit(const char *path,
               int        enable_scriptudf_protection);

#endif /* _EVAL_H_ */
