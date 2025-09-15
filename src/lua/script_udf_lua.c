#include "script_udf_lua.h"
#include "engine_lua.h"

typedef int (*lua_Udf_Loader) (lua_State *lua, const char *source, const char *name);

int luaLoadUdfModuleFile(lua_State *lua, const char *file, const char *name) {
    (void)name;
    return luaL_loadfile(lua, file);
}

int luaLoadUdfModuleString(lua_State *lua, const char *code, const char *name) {
    return luaL_loadbuffer(lua, code, strlen(code), name);
}

int luaRegisterUdfModule(lua_State *lua,
                         lua_Udf_Loader load,
                         const char *source,
                         const char *name,
                         robj **err) {
    /** Get _M table. **/
    // using lua_rawget instead of `lua_getglobal(lua, "_M")`
    lua_pushstring(lua, "_M");
    lua_rawget(lua, LUA_GLOBALSINDEX);      // { top, "_M" , _M }
    lua_remove(lua, -2);                    // { top, _M }
    if (lua_isnil(lua, -1)) {
        /** Create _M table when it is non-existent. **/
        lua_enablereadonlytable(lua, LUA_GLOBALSINDEX, 0);
        lua_pop(lua, 1);           // throw nil        { top }
        lua_newtable(lua);         // create table _M  { top, _M }
        lua_pushvalue(lua, -1);    // copy table _M    { top, _M, _M }
        lua_setglobal(lua, "_M");  //                  { top, _M }
        luaSetErrorMetatable(lua);
        lua_enablereadonlytable(lua, LUA_GLOBALSINDEX, 1);
    }

    /** Load source file & Resolve script. **/
    if (load(lua, source, name) || lua_pcall(lua, 0, 1, 0)) {  // load udf { top, _M, <udf> }
        sds error = sdscatfmt(sdsempty(), "Error loading lua module via '%s': %s", name, lua_tostring(lua, -1));
        *err = createObject(OBJ_STRING, error);
        lua_pop(lua, 1);
        return C_ERR;
    }
    /** Ensure UDF script is type of table. **/
    if (!lua_istable(lua, -1)) {
        sds error = sdscatfmt(sdsempty(), "Error loading lua module via '%s': UDF module should be of type table", name);
        *err = createObject(OBJ_STRING, error);
        return C_ERR;
    }
    /* Recursively lock all tables that can be reached from the _G._M table */
    luaSetTableProtectionRecursively(lua);

    lua_enablereadonlytable(lua, -2, 0);  // unset readonly from _M
    /** Add UDF to _M table. **/
    lua_setfield(lua, -2, name);          // { top, _M }
    lua_enablereadonlytable(lua, -1, 1);  // set readonly to _M

    lua_settop(lua, 0);
    return C_OK;
}

int luaRegisterUdfModuleFile(scriptingEngine *engine,
                             subsystemType type,
                             const char *file,
                             const char *name,
                             robj **err) {

    serverLog(LL_NOTICE, "(luaRegisterUdfModuleFile)Load UDF module file: %s, %s", file, name);
    engineCtx *ctx = extractEngineCtx(engine);
    lua_State *lua = extractLuaState(ctx, type);
    return luaRegisterUdfModule(lua, luaLoadUdfModuleFile, file, name, err);
}

int luaRegisterUdfModuleContent(scriptingEngine *engine,
                                subsystemType type,
                                const char *source,
                                const char *name,
                                robj **err) {
    serverLog(LL_NOTICE, "(luaRegisterUdfModuleContent)Load UDF module source: %s", name);
    engineCtx *ctx = extractEngineCtx(engine);
    lua_State *lua = extractLuaState(ctx, type);
    return luaRegisterUdfModule(lua, luaLoadUdfModuleString, source, name, err);
}
