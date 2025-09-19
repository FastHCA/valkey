#ifndef __SCRIPT_UDF_LUA_H_
#define __SCRIPT_UDF_LUA_H_

#include "../scripting_engine.h"
#include "script_lua.h"

int luaRegisterLibFile(scriptingEngine *engine,
                       subsystemType   type,
                       const char      *file,
                       const char      *name,
                       robj            **err);
int luaRegisterUdfModuleFile(scriptingEngine *engine,
                             subsystemType   type,
                             const char      *file,
                             const char      *name,
                             robj            **err);
int luaRegisterUdfModuleContent(scriptingEngine *engine,
                                subsystemType   type,
                                const char      *source,
                                const char      *name,
                                robj            **err);

// int getGlobalElementNames(scriptingEngine *engine,
//                           subsystemType   type,
//                           char            ***names);

int getGlobalElementNames(scriptingEngine *engine,
                          subsystemType   type,
                          robj            **names);

#endif /* __SCRIPT_UDF_LUA_H_ */
