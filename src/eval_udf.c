#include "eval.h"
// #include "server.h"
// #include "sha1.h"
// #include "rand.h"
// #include "cluster.h"
// #include "monotonic.h"
// #include "resp_parser.h"
// #include "script.h"
// #include "lua/debug_lua.h"
#include "lua/engine_lua.h"
#include "lua/script_udf_lua.h"
// #include "scripting_engine.h"

#include <dlfcn.h>
#include <dirent.h>
#include <sys/types.h> // Required for DT_REG
#include "fmtargs.h"
#include "sds.h"


struct luaLibCtx {
    char            *libDir;
    scriptingEngine *scriptingEngine;
    robj            *protectedGlobalNames;
    dict            *loadedLibs;         // key:(char*)libname, value:(void*)lib
} luaLibCtx;

struct evalUdfCtx {
    char            *udfDir;
    scriptingEngine *scriptingEngine;
    dict            *scripts;      // key:(char*)udf_name, value:(compiledFunction*)

    int enable_scriptudf_protection;
} evalUdfCtx;

typedef struct osLibExtInfo {
    const char *ext;
    size_t     length;
} osLibExtInfo;

static osLibExtInfo osLibExts[] = {
    {".so"   , 3},
    {".dylib", 6},
    {NULL    , 0}
};

static uint64_t dictCStrHash(const void *key) {
    return dictGenHashFunction((unsigned char *)key, strlen((char *)key));
}

/* Dict compare function for null terminated string */
static int dictCStrKeyCompare(const void *key1, const void *key2) {
    return strcmp(key1, key2) == 0;
}

static void dictLibFree(void *val) {
    if (val != NULL) {
        dlclose(val);
    }
}

static char *normalizeDirPath(const char *path) {
    char *out_path = (char *)path;

    // trim trailing char '/'
    int i = strlen(out_path) - 1;
    if (i > 0 && out_path[i] == '/') {
        out_path[i] = '\0';
    }
    return out_path;
}

dictType luaLibMappingDictType = {
    dictCStrHash,          /* hash function */
    NULL,                  /* key dup */
    dictCStrKeyCompare,    /* key compare */
    dictVanillaFree,       /* key destructor */
    dictLibFree,           /* val destructor */
    NULL,                  /* allow to expand */
};

dictType udfScriptMappingDictType = {
    dictCStrHash,          /* hash function */
    NULL,                  /* key dup */
    dictCStrKeyCompare,    /* key compare */
    dictVanillaFree,       /* key destructor */
    dictVanillaFree,       /* val destructor */
    NULL,                  /* allow to expand */
};

void luaLibCtxInit(sds *err) {
    scriptingEngine *engine = scriptingEngineManagerFind("lua");
    if (!engine) {
        *err = sdsnew("cannot find script engine lua");
        return;
    }

    robj *globalNames = createSetObject();
    getGlobalElementNames(engine, VMSE_EVAL, &globalNames);

    luaLibCtx.scriptingEngine      = engine;
    luaLibCtx.protectedGlobalNames = globalNames;
    luaLibCtx.loadedLibs           = dictCreate(&luaLibMappingDictType);
}

void evalUdfCtxInit(sds *err) {
    scriptingEngine *engine = scriptingEngineManagerFind("lua");
    if (!engine) {
        *err = sdsnew("cannot find script engine lua");
        return;
    }

    evalUdfCtx.scriptingEngine = engine;
    evalUdfCtx.scripts         = dictCreate(&udfScriptMappingDictType);
}

int putLuaLib(char *name, void *lib) {
    return dictReplace(luaLibCtx.loadedLibs, name, lib);
}

int addUdfScript(char *name, compiledFunction *function) {
    return dictAdd(evalUdfCtx.scripts, name, function);
}

dictEntry *lookupLuaLib(sds name) {
    return dictFind(luaLibCtx.loadedLibs, (char *)name);
}

dictEntry *lookupUdfScript(sds name) {
    return dictFind(evalUdfCtx.scripts, (char *)name);
}


void loadUdfModuleFile(scriptingEngine *engine,
                       const char      *parent_path,
                       sds             *err) {
    DIR *dir;
    struct dirent *entry;

    sds path = sdsempty();
    path = sdscatfmt(path, "%s/code", parent_path);

    // open lua UDF module dir
    dir = opendir(path);
    if (dir == NULL) {
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        // collect all ?.lua file
        if (entry->d_type == DT_REG) {
            size_t len = strlen(entry->d_name);
            if (len > 4 && strcmp(entry->d_name + (len - 4), ".lua") == 0) {
                serverLog(LL_NOTICE, "Opening UDF module file: %s", entry->d_name);

                // compose full file path
                sds file_path = sdsnew(path);
                file_path = sdscat(file_path, "/");
                file_path = sdscat(file_path, entry->d_name);

                // get module name
                size_t module_name_size = len - 4;
                char *module_name = zmalloc(sizeof(char *) * module_name_size + 1);;
                memcpy(module_name, entry->d_name, module_name_size);
                module_name[module_name_size] = '\0';

                robj *_err = NULL;
                luaRegisterUdfModuleFile(engine, VMSE_EVAL, (char *)file_path, module_name, &_err);
                zfree(module_name);
                sdsfree(file_path);
                if (_err != NULL) {
                    serverAssert(_err != NULL);

                    *err = sdscatfmt(sdsempty(), "Error loading UDF module file: %s. %s", file_path, (char *)_err->ptr);
                    decrRefCount(_err);

                    serverLog(LL_WARNING, (char *)*err);
                    break;
                }
            }
        }
    }
    closedir(dir);
}


void loadUdfScriptFile(scriptingEngine *engine,
                       const char      *path,
                       sds             *err) {
    DIR *dir;
    struct dirent *entry;

    // open lua UDF script dir
    dir = opendir(path);
    if (dir == NULL) {
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        // collect all ?.lua file
        if (entry->d_type == DT_REG) {
            size_t len = strlen(entry->d_name);
            if (len > 4 && strcmp(entry->d_name + (len - 4), ".lua") == 0) {
                serverLog(LL_NOTICE, "Opening UDF script file: %s", entry->d_name);

                // compose full file path
                sds file_path = sdsnew(path);
                file_path = sdscat(file_path, "/");
                file_path = sdscat(file_path, entry->d_name);

                // get script name
                size_t script_name_size = len - 4;
                char *script_name = zmalloc(sizeof(char *) * script_name_size + 1);
                memcpy(script_name, entry->d_name, script_name_size);
                script_name[script_name_size] = '\0';

                // read script content
                sds script_body;
                {
                    FILE *fp;
                    long buf_size;
                    char *buf;

                    fp = fopen(file_path, "rb");
                    if (fp == NULL) {
                        *err = sdscatfmt(sdsempty(), "Error opening UDF script file: %s.", file_path);
                        serverLog(LL_WARNING, (char *)*err);
                        break;
                    }

                    fseek(fp, 0, SEEK_END);
                    buf_size = ftell(fp);
                    rewind(fp);

                    buf = (char *)zmalloc(buf_size);
                    if (buf == NULL) {
                        *err = sdscatfmt(sdsempty(), "Error opening UDF script file: %s. Out of memory.", file_path);
                        serverLog(LL_WARNING, (char *)*err);

                        fclose(fp);
                        sdsfree(file_path);
                        break;
                    }
                    fread(buf, 1, buf_size, fp);

                    script_body = sdsnewlen(buf, buf_size);

                    zfree(buf);
                    fclose(fp);
                }


                robj *_err = NULL;
                size_t num_compiled_functions = 0;
                compiledFunction **functions =
                    scriptingEngineCallCompileCode(engine,
                                                   VMSE_EVAL,
                                                   script_body,
                                                   sdslen(script_body),
                                                   0,
                                                   &num_compiled_functions,
                                                   &_err);
                if (functions == NULL) {
                    serverAssert(_err != NULL);

                    *err = sdsnew((char *)_err->ptr);
                    serverLog(LL_WARNING, (char *)*err);

                    decrRefCount(_err);
                    return;
                }

                serverAssert(num_compiled_functions == 1);
                // add to evalUdfCtx.scripts
                addUdfScript(script_name, functions[0]);

                zfree(script_name);
                sdsfree(file_path);
                sdsfree(script_body);
            }
        }
    }
    closedir(dir);
    return;
}


void loadLuaLibFile(scriptingEngine *engine,
                    const char      *path,
                    sds             *err) {
    serverAssert(err != NULL);

    DIR *dir;
    struct dirent *entry;

    // open lua lib dir
    dir = opendir(path);
    if (dir == NULL) {
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        // collect all ?.so file
        if (entry->d_type == DT_REG) {
            size_t len = strlen(entry->d_name);

            int found = 0;
            osLibExtInfo info;
            for (int i = 0; ; i++) {
                info = osLibExts[i];
                if (info.ext == NULL) break;
                if (len > info.length && strcmp(entry->d_name + (len - 3), info.ext) == 0) {
                    found = 1;
                    break;
                }
            }
            if (found) {
                serverLog(LL_NOTICE, "Opening lua library file: %s", entry->d_name);

                // compose full file path
                sds file_path = sdsnew(path);
                file_path = sdscat(file_path, "/");
                file_path = sdscat(file_path, entry->d_name);

                // get module name
                size_t libname_size = len - info.length;
                char *libname = zmalloc(sizeof(char *) * libname_size + 1);
                memcpy(libname, entry->d_name, libname_size);
                libname[libname_size] = '\0';

                // check libname
                if (setTypeIsMemberAux(luaLibCtx.protectedGlobalNames, libname, libname_size, 0, 0)) {
                    serverLog(LL_WARNING,
                        "Ignored lua library file: %s. The name '%s' is protected by _G.",
                        file_path,
                        libname);
                    continue;
                }

                // register library to lua global
                void *lib = dlopen(file_path, RTLD_NOW);
                if (lib == NULL) {
                    *err = sdscatfmt(sdsempty(), "Error loading lua library file: %s. %s", file_path, dlerror());

                    zfree(libname);
                    sdsfree(file_path);

                    serverLog(LL_WARNING, (char *)*err);
                    break;
                }

                robj *_err = NULL;
                luaRegisterLibFile(engine, VMSE_EVAL, lib, libname, &_err);
                if (_err != NULL) {
                    serverAssert(_err != NULL);

                    *err = sdscatfmt(sdsempty(), "Error loading lua library file: %s. %s", file_path, (char *)_err->ptr);

                    decrRefCount(_err);
                    zfree(libname);
                    zfree(file_path);
                    dlclose(lib);

                    serverLog(LL_WARNING, (char *)*err);
                    break;
                }

                // add luaLibCtx.loadedLibs
                putLuaLib(libname, lib);

                sdsfree(file_path);
            }
        }
    }
    closedir(dir);
}


int luaLibInit(const char *lib_path) {
    sds err = NULL;
    luaLibCtxInit(&err);
    if (err) {
        serverAssert(err != NULL);

        fprintf(stderr, "%s\n", (char *)err);
        sdsfree(err);
        return C_ERR;
    }

    if (strcmp(lib_path, "")) {
        char *path = normalizeDirPath(lib_path);

        luaLibCtx.libDir = path;

        serverLog(LL_NOTICE, "Opening lua library path: %s", path);

        scriptingEngine *engine = luaLibCtx.scriptingEngine;
        if (!engine) {
            fprintf(stderr, "%s\n", "Cannot find script engine lua");
            return C_ERR;
        }

        loadLuaLibFile(engine, path, &err);
        if (err) {
            serverAssert(err != NULL);

            fprintf(stderr, "%s\n", (char *)err);
            sdsfree(err);
            return C_ERR;
        }
    }
    return C_OK;
}


int luaUdfInit(const char *udf_path,
               int enable_scriptudf_protection) {
    sds err = NULL;
    evalUdfCtxInit(&err);
    if (err) {
        serverAssert(err != NULL);

        fprintf(stderr, "%s\n", (char *)err);
        sdsfree(err);
        return C_ERR;
    }
    evalUdfCtx.enable_scriptudf_protection = enable_scriptudf_protection;

    if (strcmp(udf_path, "")) {
        char *path = normalizeDirPath(udf_path);

        evalUdfCtx.udfDir = path;

        serverLog(LL_NOTICE, "Opening UDF path: %s", path);

        scriptingEngine *engine = evalUdfCtx.scriptingEngine;
        if (!engine) {
            fprintf(stderr, "%s\n", "Cannot find script engine lua");
            return C_ERR;
        }

        loadUdfModuleFile(engine, path, &err);
        if (err) {
            serverAssert(err != NULL);

            fprintf(stderr, "%s\n", (char *)err);
            sdsfree(err);
            return C_ERR;
        }

        loadUdfScriptFile(engine, path, &err);
        if (err) {
            serverAssert(err != NULL);

            fprintf(stderr, "%s\n", (char *)err);
            sdsfree(err);
            return C_ERR;
        }
    }
    return C_OK;
}


static int reloadUdf(client *c) {
    const char      *path   = evalUdfCtx.udfDir;
    scriptingEngine *engine = evalUdfCtx.scriptingEngine;

    if (!path) {
        return C_OK;
    }
    if (!engine) {
        addReplyError(c, "Cannot find script engine lua");
        return C_ERR;
    }

    if (strcmp(path, "")) {
        serverLog(LL_NOTICE, "Load UDF via path '%s'", path);

        sds err = NULL;
        loadUdfModuleFile(engine, path, &err);
        if (err) {
            serverAssert(err != NULL);

            addReplyError(c, err);
            return C_ERR;
        }

        loadUdfScriptFile(engine, path, &err);
        if (err) {
            serverAssert(err != NULL);

            addReplyError(c, err);
            return C_ERR;
        }
    }
    return C_OK;
}


static int installUdfModuleSource(client *c, robj *name, robj *body) {
    if (evalUdfCtx.enable_scriptudf_protection) {
        addReplyError(c, "Deny 'SCRIPTUDF INSTALL' command. The enable-scriptudf-protection is on.");
        return C_ERR;
    }

    scriptingEngine *engine = evalUdfCtx.scriptingEngine;

    if (!engine) {
        addReplyError(c, "Cannot find script engine lua");
        return C_ERR;
    }

    robj *_err = NULL;
    luaRegisterUdfModuleContent(engine, VMSE_EVAL, body->ptr, name->ptr, &_err);
    if (_err != NULL) {
        serverAssert(_err != NULL);

        sds err = sdscatfmt(sdsempty(), "Cannot install UDF module. %s", (char *)_err->ptr);
        addReplyErrorSds(c, err);
        serverLog(LL_WARNING, (char *)err);

        decrRefCount(_err);
        return C_ERR;
    }

    return C_OK;
}


static int registerUdfScriptSource(client *c, robj *name, robj *body) {
    if (evalUdfCtx.enable_scriptudf_protection) {
        addReplyError(c, "Deny 'SCRIPTUDF LOAD' command. The enable-scriptudf-protection is on.");
        return C_ERR;
    }

    scriptingEngine *engine = evalUdfCtx.scriptingEngine;

    if (!engine) {
        addReplyError(c, "Cannot find script engine lua");
        return C_ERR;
    }

    robj *_err = NULL;
    size_t num_compiled_functions = 0;
    compiledFunction **functions =
        scriptingEngineCallCompileCode(engine,
                                       VMSE_EVAL,
                                       (sds)body->ptr,
                                       sdslen(body->ptr),
                                       0,
                                       &num_compiled_functions,
                                       &_err);
    if (functions == NULL) {
        serverAssert(_err != NULL);

        sds err = sdscatfmt(sdsempty(), "Cannot install UDF script. %s", (char *)_err->ptr);
        addReplyErrorSds(c, err);
        serverLog(LL_WARNING, (char *)err);

        decrRefCount(_err);
        return C_ERR;
    }

    serverAssert(num_compiled_functions == 1);
    // add to evalUdfCtx.scripts
    {
        // get script name
        size_t script_name_size = sdslen(name->ptr);
        char *script_name = zmalloc(sizeof(char *) * script_name_size + 1);
        memcpy(script_name, name->ptr, script_name_size);
        script_name[script_name_size] = '\0';

        addUdfScript(script_name, functions[0]);

        zfree(script_name);
    }

    return C_OK;
}


/* Try to extract command flags if we can, returns the modified flags.
 * Note that it does not guarantee the command arguments are right. */
uint64_t evalUdfGetCommandFlags(client *c, uint64_t cmd_flags) {
    robj *function_name = c->argv[1];

    c->cur_script = lookupUdfScript(function_name->ptr);
    if (!c->cur_script) return cmd_flags;

    compiledFunction *compiled_function = dictGetVal(c->cur_script);
    uint64_t script_flags = compiled_function->f_flags;
    if (script_flags & SCRIPT_FLAG_EVAL_COMPAT_MODE) return cmd_flags;
    return scriptFlagsToCmdFlags(cmd_flags, script_flags);
}


static void evalUdfScript(client *c, int ro) {
    /* Explicitly feed monitor here so that lua commands appear after their
     * script command. */
    replicationFeedMonitors(c, server.monitors, c->db->id, c->argv, c->argc);

    scriptingEngine *engine = evalUdfCtx.scriptingEngine;
    if (!engine) {
        addReplyError(c, "Cannot find script engine lua");
        return;
    }

    robj *function_name = c->argv[1];
    long long numkeys;
    /* Get the number of arguments that are keys */
    if (getLongLongFromObjectOrReply(c, c->argv[2], &numkeys, NULL) != C_OK) return;
    if (numkeys > (c->argc - 3)) {
        addReplyError(c, "Number of keys can't be greater than number of args");
        return;
    } else if (numkeys < 0) {
        addReplyError(c, "Number of keys can't be negative");
        return;
    }

    dictEntry *de = c->cur_script;
    if (de == NULL) de = lookupUdfScript(function_name->ptr);
    if (de == NULL) {
        /* Calling EVALUDF using an name that was never added to the scripts
         * cache. */
        addReplyErrorObject(c, shared.noscripterr);
        return;
    }
    compiledFunction *compiled_function = dictGetVal(de);

    scriptRunCtx rctx;
    if (scriptPrepareForRun(&rctx,
                            scriptingEngineGetClient(engine),
                            c,
                            (sds)function_name->ptr,
                            compiled_function->f_flags,
                            ro) != C_OK) return;
    rctx.flags |= SCRIPT_EVAL_MODE;

    scriptingEngineCallFunction(engine,
                                &rctx,
                                c, // rctx.original_client
                                compiled_function,
                                VMSE_EVAL,
                                c->argv + 3,
                                numkeys,
                                c->argv + 3 + numkeys,
                                c->argc - 3 - numkeys);
    scriptResetRun(&rctx);
}


void evalUdfCommand(client *c) {
    evalUdfScript(c, 0);
}


sds genUdfInfoString(sds info) {
    return sdscatprintf(
        info,
        FMTARGS(
            "scriptudf_dir:%s\r\n"              , (evalUdfCtx.udfDir ? evalUdfCtx.udfDir : ""),
            "enable_scriptudf_protection:%s\r\n", (evalUdfCtx.enable_scriptudf_protection ? "yes" : "no")));
}


void scriptUdfModuleCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr, "restore")) {  // SCRIPTUDF RESTORE
        if (reloadUdf(c) == C_ERR) {
            return;
        }
        addReply(c, shared.ok);
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr, "list")) {
        addReplyArrayLen(c, dictSize(evalUdfCtx.scripts));
        dictIterator *di = dictGetIterator(evalUdfCtx.scripts);
        dictEntry *de;
        while ((de = dictNext(di)) != NULL) {
            addReplyBulkCString(c, (char *)dictGetKey(de));
        }
        dictReleaseIterator(di);
    } else if (c->argc == 4 && !strcasecmp(c->argv[1]->ptr, "install")) {
        if (installUdfModuleSource(c, c->argv[2], c->argv[3]) != C_OK) {
            return;
        }
        addReply(c, shared.ok);
    } else if (c->argc == 4 && !strcasecmp(c->argv[1]->ptr, "load")) {
        if (registerUdfScriptSource(c, c->argv[2], c->argv[3]) != C_OK) {
            return;
        }
        addReply(c, shared.ok);
    } else if (c->argc >= 2 && !strcasecmp(c->argv[1]->ptr, "exists")) {
        int j;

        addReplyArrayLen(c, c->argc - 2);
        for (j = 2; j < c->argc; j++) {
            if (lookupUdfScript(c->argv[j]->ptr))
                addReply(c, shared.cone);
            else
                addReply(c, shared.czero);
        }
    } else {
        addReplySubcommandSyntaxError(c);
    }
}


sds genLuaLibInfoString(sds info) {
    return sdscatprintf(
        info,
        FMTARGS(
            "lualib_dir:%s\r\n", (luaLibCtx.libDir ? luaLibCtx.libDir : ""),
            "loaded_libraries:%ld\r\n", dictSize(luaLibCtx.loadedLibs)));
}


void luaLibCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr, "list")) {
        addReplyArrayLen(c, dictSize(luaLibCtx.loadedLibs));
        dictIterator *di = dictGetIterator(luaLibCtx.loadedLibs);
        dictEntry *de;
        while ((de = dictNext(di)) != NULL) {
            addReplyBulkCString(c, (char *)dictGetKey(de));
        }
        dictReleaseIterator(di);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

