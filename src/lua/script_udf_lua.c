#include "script_udf_lua.h"
#include "engine_lua.h"

#define GLOBAL_UDF_MODULE_NAME "_M"

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
    // using lua_rawget instead of `lua_getglobal(lua, GLOBAL_UDF_MODULE_NAME)`
    lua_pushstring(lua, GLOBAL_UDF_MODULE_NAME);
    lua_rawget(lua, LUA_GLOBALSINDEX);      // { top, "_M" , _M }
    lua_remove(lua, -2);                    // { top, _M }
    if (lua_isnil(lua, -1)) {
        /** Create _M table when it is non-existent. **/
        lua_enablereadonlytable(lua, LUA_GLOBALSINDEX, 0);
        lua_pop(lua, 1);           // throw nil        { top }
        lua_newtable(lua);         // create table _M  { top, _M }
        lua_pushvalue(lua, -1);    // copy table _M    { top, _M, _M }
        lua_setglobal(lua, GLOBAL_UDF_MODULE_NAME);  //                  { top, _M }
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

    lua_enablereadonlytable(lua, -2, 0);  // unset readonly to _M
    /** Add UDF to _M table. **/
    lua_setfield(lua, -2, name);          // { top, _M }
    lua_enablereadonlytable(lua, -1, 1);  // set readonly to _M

    lua_settop(lua, 0);
    return C_OK;
}

int luaRegisterLib(lua_State *lua,
                   const char *file,
                   const char *name,
                   robj **err) {

    lua_pushvalue(lua, LUA_GLOBALSINDEX);   // { top, _G }
    lua_enablereadonlytable(lua, LUA_GLOBALSINDEX, 0);  // unset readonly to _G

    if (luaL_loadlib(lua, file, name)) {    // { top, _G, <cfunc> }
        sds error = sdscatfmt(sdsempty(), "Error loading lua library via '%s': %s", file, lua_tostring(lua, -1));
        *err = createObject(OBJ_STRING, error);
        goto label_done;
    }
    lua_pushstring(lua, name);              // { top, _G, <cfunc>, <name> }
    if (lua_pcall(lua, 1, 0, 0)) {          // { top, _G, <cfunc>, <name> }
        sds error = sdscatfmt(sdsempty(), "Error loading lua library via '%s': %s", file, lua_tostring(lua, -1));
        *err = createObject(OBJ_STRING, error);
        goto label_done;
    }

label_done:
    lua_enablereadonlytable(lua, LUA_GLOBALSINDEX, 1);  // set readonly to _G
    lua_settop(lua, 0);

    if (*err != NULL) {
        return C_ERR;
    }
    return C_OK;
}


int luaRegisterLibFile(scriptingEngine *engine,
                       subsystemType type,
                       const char *file,
                       const char *name,
                       robj **err) {
    serverLog(LL_NOTICE, "Loading lua library '%s' from file: %s", name, file);
    engineCtx *ctx = extractEngineCtx(engine);
    lua_State *lua = extractLuaState(ctx, type);
    return luaRegisterLib(lua, file, name, err);
}


int luaRegisterUdfModuleFile(scriptingEngine *engine,
                             subsystemType type,
                             const char *file,
                             const char *name,
                             robj **err) {
    serverLog(LL_NOTICE, "Loading UDF module '%s' from file: %s", name, file);
    engineCtx *ctx = extractEngineCtx(engine);
    lua_State *lua = extractLuaState(ctx, type);
    return luaRegisterUdfModule(lua, luaLoadUdfModuleFile, file, name, err);
}


int luaRegisterUdfModuleContent(scriptingEngine *engine,
                                subsystemType type,
                                const char *source,
                                const char *name,
                                robj **err) {
    serverLog(LL_NOTICE, "Loading UDF module '%s' from source", name);
    engineCtx *ctx = extractEngineCtx(engine);
    lua_State *lua = extractLuaState(ctx, type);
    return luaRegisterUdfModule(lua, luaLoadUdfModuleString, source, name, err);
}


int getGlobalElementNames(scriptingEngine *engine,
                          subsystemType type,
                          robj **out_names) {
    serverAssert(out_names != NULL);
    serverAssert(*out_names != NULL);
    serverAssert((*out_names)->type == OBJ_SET);

    engineCtx *ctx = extractEngineCtx(engine);
    lua_State *lua = extractLuaState(ctx, type);

    robj *names = *out_names;
    int count = 0;

    lua_pushvalue(lua, LUA_GLOBALSINDEX);  // { top, _G }
    // Push the first key for iteration (nil)
    lua_pushnil(lua);                      // { top, _G, <key>nil }
    // Loop through the table
    while (lua_next(lua, -2) != 0) {       // { top, _G, <key>, <value> }
        // The key is at -2, the value is at -1
        if (lua_type(lua, -2) == LUA_TSTRING) {
            count++;
            // add the key to <out_names>
            setTypeAdd(names, sdsnew(lua_tostring(lua, -2)));
        }
        // Pop the value leaving the key on top for the next iteration
        lua_pop(lua, 1);                   // { top, _G, <key> }
    } // { top, _G }

    lua_settop(lua, 0);

    setTypeAdd(names, sdsnew(GLOBAL_UDF_MODULE_NAME));
    setTypeAdd(names, sdsnew("ARGV"));
    setTypeAdd(names, sdsnew("KEYS"));
    return count;
}
