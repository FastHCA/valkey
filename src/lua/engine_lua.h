#ifndef _ENGINE_LUA_
#define _ENGINE_LUA_

#include "../scripting_engine.h"
#include <lua.h>

int luaEngineInitEngine(void);

/* helper */
lua_State *extractLuaState(engineCtx *engine_ctx, subsystemType type);

#endif /* _ENGINE_LUA_ */
