﻿/*
** $Id: loadlib.c,v 1.127 2015/11/23 11:30:45 roberto Exp $
** Dynamic library loader for Lua
** See Copyright Notice in lua.h
**
** This module contains an implementation of loadlib for Unix systems
** that have dlfcn, an implementation for Windows, and a stub for other
** systems.
*/

#define loadlib_cpp

#include <uvm/lprefix.h>


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include <uvm/lua.h>
#include <uvm/lapi.h>
#include <uvm/lauxlib.h>
#include <uvm/lualib.h>
#include <uvm/uvm_api.h>
#include <uvm/uvm_lib.h>
#include <uvm/uvm_lutil.h>

using uvm::lua::api::global_uvm_chain_api;


/*
** LUA_PATH_VAR and LUA_CPATH_VAR are the names of the environment
** variables that Lua check to set its paths.
*/
#if !defined(LUA_PATH_VAR)
#define LUA_PATH_VAR	"LUA_PATH"
#endif

#if !defined(LUA_CPATH_VAR)
#define LUA_CPATH_VAR	"LUA_CPATH"
#endif

#define LUA_PATHSUFFIX		"_" LUA_VERSION_MAJOR "_" LUA_VERSION_MINOR

#define LUA_PATHVARVERSION		LUA_PATH_VAR LUA_PATHSUFFIX
#define LUA_CPATHVARVERSION		LUA_CPATH_VAR LUA_PATHSUFFIX

/*
** LUA_PATH_SEP is the character that separates templates in a path.
** LUA_PATH_MARK is the string that marks the substitution points in a
** template.
** LUA_EXEC_DIR in a Windows path is replaced by the executable's
** directory.
** LUA_IGMARK is a mark to ignore all before it when building the
** luaopen_ function name.
*/
#if !defined (LUA_PATH_SEP)
#define LUA_PATH_SEP		";"
#endif
#if !defined (LUA_PATH_MARK)
#define LUA_PATH_MARK		"?"
#endif
#if !defined (LUA_EXEC_DIR)
#define LUA_EXEC_DIR		"!"
#endif
#if !defined (LUA_IGMARK)
#define LUA_IGMARK		"-"
#endif


/*
** LUA_CSUBSEP is the character that replaces dots in submodule names
** when searching for a C loader.
** LUA_LSUBSEP is the character that replaces dots in submodule names
** when searching for a Lua loader.
*/
#if !defined(LUA_CSUBSEP)
#define LUA_CSUBSEP		LUA_DIRSEP
#endif

#if !defined(LUA_LSUBSEP)
#define LUA_LSUBSEP		LUA_DIRSEP
#endif


/* prefix for open functions in C libraries */
#define LUA_POF		"luaopen_"

/* separator for open functions in C libraries */
#define LUA_OFSEP	"_"


/*
** unique key for table in the registry that keeps handles
** for all loaded C libraries
*/
static const int CLIBS = 0;

#define LIB_FAIL	"open"

#define setprogdir(L)		((void)0)


/*
** system-dependent functions
*/

/*
** unload library 'lib'
*/
static void lsys_unloadlib(void *lib);

/*
** load C library in file 'path'. If 'seeglb', load with all names in
** the library global.
** Returns the library; in case of error, returns nullptr plus an
** error string in the stack.
*/
static void *lsys_load(lua_State *L, const char *path, int seeglb);

/*
** Try to find a function named 'sym' in library 'lib'.
** Returns the function; in case of error, returns nullptr plus an
** error string in the stack.
*/
static lua_CFunction lsys_sym(lua_State *L, void *lib, const char *sym);




#if defined(LUA_USE_DLOPEN)	/* { */
/*
** {========================================================================
** This is an implementation of loadlib based on the dlfcn interface.
** The dlfcn interface is available in Linux, SunOS, Solaris, IRIX, FreeBSD,
** NetBSD, AIX 4.2, HPUX 11, and  probably most other Unix flavors, at least
** as an emulation layer on top of native functions.
** =========================================================================
*/

#include <dlfcn.h>

/*
** Macro to convert pointer-to-void* to pointer-to-function. This cast
** is undefined according to ISO C, but POSIX assumes that it works.
** (The '__extension__' in gnu compilers is only to avoid warnings.)
*/
#if defined(__GNUC__)
#define cast_func(p) (__extension__ (lua_CFunction)(p))
#else
#define cast_func(p) ((lua_CFunction)(p))
#endif


static void lsys_unloadlib(void *lib) {
    dlclose(lib);
}


static void *lsys_load(lua_State *L, const char *path, int seeglb) {
    void *lib = dlopen(path, RTLD_NOW | (seeglb ? RTLD_GLOBAL : RTLD_LOCAL));
    if (lib == nullptr) lua_pushstring(L, dlerror());
    return lib;
}


static lua_CFunction lsys_sym(lua_State *L, void *lib, const char *sym) {
    lua_CFunction f = cast_func(dlsym(lib, sym));
    if (f == nullptr) lua_pushstring(L, dlerror());
    return f;
}

/* }====================================================== */



#elif defined(LUA_DL_DLL)	/* }{ */
/*
** {======================================================================
** This is an implementation of loadlib for Windows using native functions.
** =======================================================================
*/

#include <windows.h>

#undef setprogdir

/*
** optional flags for LoadLibraryEx
*/
#if !defined(LUA_LLE_FLAGS)
#define LUA_LLE_FLAGS	0
#endif


static void setprogdir(lua_State *L) {
    char buff[MAX_PATH + 1];
    char *lb;
    DWORD nsize = sizeof(buff) / sizeof(char);
    DWORD n = GetModuleFileNameA(nullptr, buff, nsize);
    if (n == 0 || n == nsize || (lb = strrchr(buff, '\\')) == nullptr)
        luaL_error(L, "unable to get ModuleFileName");
    else {
        *lb = '\0';
        luaL_gsub(L, lua_tostring(L, -1), LUA_EXEC_DIR, buff);
        lua_remove(L, -2);  /* remove original string */
    }
}


static void pusherror(lua_State *L) {
    int error = GetLastError();
    char buffer[128];
    if (FormatMessageA(FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM,
        nullptr, error, 0, buffer, sizeof(buffer) / sizeof(char), nullptr))
        lua_pushstring(L, buffer);
    else
        lua_pushfstring(L, "system error %d\n", error);
}

static void lsys_unloadlib(void *lib) {
    FreeLibrary((HMODULE)lib);
}


static void *lsys_load(lua_State *L, const char *path, int seeglb) {
    HMODULE lib = LoadLibraryExA(path, nullptr, LUA_LLE_FLAGS);
    (void)(seeglb);  /* not used: symbols are 'global' by default */
    if (lib == nullptr) pusherror(L);
    return lib;
}


static lua_CFunction lsys_sym(lua_State *L, void *lib, const char *sym) {
    lua_CFunction f = (lua_CFunction)GetProcAddress((HMODULE)lib, sym);
    if (f == nullptr) pusherror(L);
    return f;
}

/* }====================================================== */


#else				/* }{ */
/*
** {======================================================
** Fallback for other systems
** =======================================================
*/

#undef LIB_FAIL
#define LIB_FAIL	"absent"


#define DLMSG	"dynamic libraries not enabled; check your Lua installation"


static void lsys_unloadlib(void *lib) {
    (void)(lib);  /* not used */
}


static void *lsys_load(lua_State *L, const char *path, int seeglb) {
    (void)(path); (void)(seeglb);  /* not used */
    lua_pushliteral(L, DLMSG);
    return nullptr;
}


static lua_CFunction lsys_sym(lua_State *L, void *lib, const char *sym) {
    (void)(lib); (void)(sym);  /* not used */
    lua_pushliteral(L, DLMSG);
    return nullptr;
}

/* }====================================================== */
#endif				/* } */


/*
** return registry.CLIBS[path]
*/
static void *checkclib(lua_State *L, const char *path) {
    void *plib;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &CLIBS);
    lua_getfield(L, -1, path);
    plib = lua_touserdata(L, -1);  /* plib = CLIBS[path] */
    lua_pop(L, 2);  /* pop CLIBS table and 'plib' */
    return plib;
}


/*
** registry.CLIBS[path] = plib        -- for queries
** registry.CLIBS[#CLIBS + 1] = plib  -- also keep a list of all libraries
*/
static void addtoclib(lua_State *L, const char *path, void *plib) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &CLIBS);
    lua_pushlightuserdata(L, plib);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, path);  /* CLIBS[path] = plib */
    lua_rawseti(L, -2, luaL_len(L, -2) + 1);  /* CLIBS[#CLIBS + 1] = plib */
    lua_pop(L, 1);  /* pop CLIBS table */
}


/*
** __gc tag method for CLIBS table: calls 'lsys_unloadlib' for all lib
** handles in list CLIBS
*/
static int gctm(lua_State *L) {
    lua_Integer n = luaL_len(L, 1);
    for (; n >= 1; n--) {  /* for each handle, in reverse order */
        lua_rawgeti(L, 1, n);  /* get handle CLIBS[n] */
        lsys_unloadlib(lua_touserdata(L, -1));
        lua_pop(L, 1);  /* pop handle */
    }
    return 0;
}



/* error codes for 'lookforfunc' */
#define ERRLIB		1
#define ERRFUNC		2

/*
** Look for a C function named 'sym' in a dynamically loaded library
** 'path'.
** First, check whether the library is already loaded; if not, try
** to load it.
** Then, if 'sym' is '*', return true (as library has been loaded).
** Otherwise, look for symbol 'sym' in the library and push a
** C function with that symbol.
** Return 0 and 'true' or a function in the stack; in case of
** errors, return an error code and an error message in the stack.
*/
static int lookforfunc(lua_State *L, const char *path, const char *sym) {
    void *reg = checkclib(L, path);  /* check loaded C libraries */
    if (reg == nullptr) {  /* must load library? */
        reg = lsys_load(L, path, *sym == '*');  /* global symbols if 'sym'=='*' */
        if (reg == nullptr) return ERRLIB;  /* unable to load library */
        addtoclib(L, path, reg);
    }
    if (*sym == '*') {  /* loading only library (no function)? */
        lua_pushboolean(L, 1);  /* return 'true' */
        return 0;  /* no errors */
    }
    else {
        lua_CFunction f = lsys_sym(L, reg, sym);
        if (f == nullptr)
            return ERRFUNC;  /* unable to find function */
        lua_pushcfunction(L, f);  /* else create new function */
        return 0;  /* no errors */
    }
}


static int ll_loadlib(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    const char *init = luaL_checkstring(L, 2);
    int stat = lookforfunc(L, path, init);
    if (stat == 0)  /* no errors? */
        return 1;  /* return the loaded function */
    else {  /* error; error message is on stack top */
        lua_pushnil(L);
        lua_insert(L, -2);
        lua_pushstring(L, (stat == ERRLIB) ? LIB_FAIL : "init");
        return 3;  /* return nil, error message, and where */
    }
}



/*
** {======================================================
** 'require' function
** =======================================================
*/


static int readable(const char *filename) {
    FILE *f = fopen(filename, "r");  /* try to open file */
    if (f == nullptr) return 0;  /* open failed */
    fclose(f);
    return 1;
}


static const char *pushnexttemplate(lua_State *L, const char *path) {
    const char *l;
    while (*path == *LUA_PATH_SEP) path++;  /* skip separators */
    if (*path == '\0') return nullptr;  /* no more templates */
    l = strchr(path, *LUA_PATH_SEP);  /* find next separator */
    if (l == nullptr) l = path + strlen(path);
    lua_pushlstring(L, path, l - path);  /* template */
    return l;
}


static const char *searchpath(lua_State *L, const char *name,
    const char *path,
    const char *sep,
    const char *dirsep) {
    //if (uvm::util::starts_with(name, ADDRESS_CONTRACT_PREFIX))
    //	return nullptr;
    luaL_Buffer msg;  /* to build error message */
    luaL_buffinit(L, &msg);
    if (*sep != '\0')  /* non-empty separator? */
        name = luaL_gsub(L, name, sep, dirsep);  /* replace it by 'dirsep' */
    while ((path = pushnexttemplate(L, path)) != nullptr) { // lua_real_execute_contract_apiapi_name
        const char *filename = luaL_gsub(L, lua_tostring(L, -1),
            LUA_PATH_MARK, name);
        lua_remove(L, -2);  /* remove path template */
        if (readable(filename))  /* does file exist and is readable? */
            return filename;  /* return that file name */
        lua_pushfstring(L, "\n\tno file '%s'", filename);
        lua_remove(L, -2);  // remove file name 
        luaL_addvalue(&msg);  /* concatenate error msg. entry */
    }
    luaL_pushresult(&msg);  /* create error message */
    return nullptr;  /* not found */
}

static int checkload(lua_State *L, int stat, const char *filename) {
    if (stat) {  /* module loaded successfully? */
        lua_pushstring(L, filename);  /* will be 2nd argument to module */
        return 2;  /* return open function and file name */
    }
    else
        return luaL_error(L, "error loading module '%s' from file '%s':\n\t%s",
        lua_tostring(L, 1), filename, lua_tostring(L, -1));
}

/**
 * module searcher from uvm api
 */
static int searcher_uvm(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    if (!name)
        return LUA_ERRERR;
    char error[LUA_VM_EXCEPTION_STRNG_MAX_LENGTH];
    memset(error, 0x0, sizeof(error));
    std::string origin_contract_name_str = uvm::lua::lib::unwrap_contract_name(name);
    const char *origin_contract_name = origin_contract_name_str.c_str();
    auto stream = lua_common_open_contract(L, origin_contract_name, error);
    if (strlen(L->compile_error) < 1 && strlen(error) > 0)
    {
        memcpy(L->compile_error, error, sizeof(char)*(strlen(error) + 1));
    }
    if (!stream)
    {
        global_uvm_chain_api->throw_exception(L, UVM_API_SIMPLE_ERROR, "load contract %s error", origin_contract_name);
        return 1;
    }
    struct StreamScope {
        lua_State *L;
        UvmModuleByteStream *stream;
        const char *name;
        StreamScope(lua_State *L, const char *name, UvmModuleByteStream *stream) {
            this->name = name; this->L = L; this->stream = stream;
        }
        ~StreamScope() {
            if (!uvm::util::starts_with(std::string(name), std::string(STREAM_CONTRACT_PREFIX)))
            {
                // uvm::lua::api::free_contract(L, stream);
            }
        }
    } stream_scope(L, name, stream.get());
    uvm_types::GcLClosure *closure = uvm::lua::lib::luaU_undump_from_stream(L, stream.get(), uvm::lua::lib::unwrap_any_contract_name(origin_contract_name).c_str());
	if (!closure)
	{
		return 1;
	}
#if(CHECK_CONTRACT_CODE_EVERY_TIME)
    if (!uvm::lua::lib::check_contract_proto(L, closure->p, error))
    {
        if (strlen(L->compile_error) < 1)
        {
            memcpy(L->compile_error, error, sizeof(char)*(strlen(error) + 1));
        }
        global_uvm_chain_api->throw_exception(L, UVM_API_SIMPLE_ERROR, error ? error : "contract bytecode stream error");
        return 1;
    }
#endif

    return checkload(L, (luaL_loadbufferx(L, stream->buff.data(), stream->buff.size(), stream->is_bytes ? "binary" : "text", nullptr) == LUA_OK), name);
}


/*
** Try to find a load function for module 'modname' at file 'filename'.
** First, change '.' to '_' in 'modname'; then, if 'modname' has
** the form X-Y (that is, it has an "ignore mark"), build a function
** name "luaopen_X" and look for it. (For compatibility, if that
** fails, it also tries "luaopen_Y".) If there is no ignore mark,
** look for a function named "luaopen_modname".
*/
static int loadfunc(lua_State *L, const char *filename, const char *modname) {
    const char *openfunc;
    const char *mark;
    modname = luaL_gsub(L, modname, ".", LUA_OFSEP);
    mark = strchr(modname, *LUA_IGMARK);
    if (mark) {
        int stat;
        openfunc = lua_pushlstring(L, modname, mark - modname);
        openfunc = lua_pushfstring(L, LUA_POF"%s", openfunc);
        stat = lookforfunc(L, filename, openfunc);
        if (stat != ERRFUNC) return stat;
        modname = mark + 1;  /* else go ahead and try old-style name */
    }
    openfunc = lua_pushfstring(L, LUA_POF"%s", modname);
    return lookforfunc(L, filename, openfunc);
}

static int searcher_preload(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "_PRELOAD");
    if (lua_getfield(L, -1, name) == LUA_TNIL)  /* not found? */
        lua_pushfstring(L, "\n\tno field package.preload['%s']", name);
    return 1;
}


static void findloader(lua_State *L, const char *name) {
    int i;
    luaL_Buffer msg;  /* to build error message */
    luaL_buffinit(L, &msg);
    /* push 'package.searchers' to index 3 in the stack */
    if (lua_getfield(L, lua_upvalueindex(1), "searchers") != LUA_TTABLE)
        luaL_error(L, "'package.searchers' must be a table");
    /*  iterate over available searchers to find a loader */
    for (i = 1;; i++) {
        if (lua_rawgeti(L, 3, i) == LUA_TNIL) {  /* no more searchers? */
            lua_pop(L, 1);  /* remove nil */
            luaL_pushresult(&msg);  /* create error message */
            luaL_error(L, "module '%s' not found:%s", name, lua_tostring(L, -1));
        }
        lua_pushstring(L, name);
        lua_call(L, 1, 2);  /* call it */
        if (lua_isfunction(L, -2))  /* did it find a loader? */
            return;  /* module loader found */
        else if (lua_isstring(L, -2)) {  /* searcher returned error message? */
            lua_pop(L, 1);  /* remove extra return */
            luaL_addvalue(&msg);  /* concatenate error message */
        }
        else
            lua_pop(L, 2);  /* remove both returns */
    }
}

static void findloader_for_import_contract(lua_State *L, const char *name)
{
    int i;
    luaL_Buffer msg;  /* to build error message */
    luaL_buffinit(L, &msg);
    /* push 'package.searchers' to index 3 in the stack */
    if (lua_getfield(L, lua_upvalueindex(1), "searchers") != LUA_TTABLE)
        luaL_error(L, "'package.searchers' must be a table");
    /*  iterate over available searchers to find a loader */
    for (i = 1;; i++) {
        if (lua_rawgeti(L, 3, i) == LUA_TNIL) {  /* no more searchers? */
            lua_pop(L, 1);  /* remove nil */
            luaL_pushresult(&msg);  /* create error message */
            luaL_error(L, "module '%s' not found:%s", name, lua_tostring(L, -1));
        }
        lua_pushstring(L, name);
        lua_call(L, 1, 2);  /* call it */
        if (lua_isfunction(L, -2))  /* did it find a loader? */
            return;  /* module loader found */
        else if (lua_isstring(L, -2)) {  /* searcher returned error message? */
            lua_pop(L, 1);  /* remove extra return */
            luaL_addvalue(&msg);  /* concatenate error message */
        }
        else
            lua_pop(L, 2);  /* remove both returns */
    }
}


/**
 * require function
 */
static int ll_require(lua_State *L) {
    return luaL_require_module(L);
}

/**
* import_contract function
*/
static int ll_import_contract(lua_State *L) {
    return luaL_import_contract_module(L);
}

static int ll_import_contract_from_address(lua_State *L)
{
    return luaL_import_contract_module_from_address(L);
}

/* }====================================================== */



/*
** {======================================================
** 'module' function
** =======================================================
*/
#if defined(LUA_COMPAT_MODULE)

/*
** changes the environment variable of calling function
*/
static void set_env(lua_State *L) {
    lua_Debug ar;
    if (lua_getstack(L, 1, &ar) == 0 ||
        lua_getinfo(L, "f", &ar) == 0 ||  /* get calling function */
        lua_iscfunction(L, -1))
        luaL_error(L, "'module' not called from a Lua function");
    lua_pushvalue(L, -2);  /* copy new environment table to top */
    lua_setupvalue(L, -2, 1);
    lua_pop(L, 1);  /* remove function */
}


static void dooptions(lua_State *L, int n) {
    int i;
    for (i = 2; i <= n; i++) {
        if (lua_isfunction(L, i)) {  /* avoid 'calling' extra info. */
            lua_pushvalue(L, i);  /* get option (a function) */
            lua_pushvalue(L, -2);  /* module */
            lua_call(L, 1, 0);
        }
    }
}


static void modinit(lua_State *L, const char *modname) {
    const char *dot;
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "_M");  /* module._M = module */
    lua_pushstring(L, modname);
    lua_setfield(L, -2, "_NAME");
    dot = strrchr(modname, '.');  /* look for last dot in module name */
    if (dot == nullptr) dot = modname;
    else dot++;
    /* set _PACKAGE as package name (full module name minus last part) */
    lua_pushlstring(L, modname, dot - modname);
    lua_setfield(L, -2, "_PACKAGE");
}


static int ll_module(lua_State *L) {
    const char *modname = luaL_checkstring(L, 1);
    int lastarg = lua_gettop(L);  /* last parameter */
    luaL_pushmodule(L, modname, 1);  /* get/create module table */
    /* check whether table already has a _NAME field */
    if (lua_getfield(L, -1, "_NAME") != LUA_TNIL)
        lua_pop(L, 1);  /* table is an initialized module */
    else {  /* no; initialize it */
        lua_pop(L, 1);
        modinit(L, modname);
    }
    lua_pushvalue(L, -1);
    set_env(L);
    dooptions(L, lastarg);
    return 1;
}


static int ll_seeall(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    if (!lua_getmetatable(L, 1)) {
        lua_createtable(L, 0, 1); /* create new metatable */
        lua_pushvalue(L, -1);
        lua_setmetatable(L, 1);
    }
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");  /* mt.__index = _G */
    return 0;
}

#endif
/* }====================================================== */



/* auxiliary mark (for internal use) */
#define AUXMARK		"\1"


/*
** return registry.LUA_NOENV as a boolean
*/
static int noenv(lua_State *L) {
    int b;
    lua_getfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");
    b = lua_toboolean(L, -1);
    lua_pop(L, 1);  /* remove value */
    return b;
}


static void setpath(lua_State *L, const char *fieldname, const char *envname1,
    const char *envname2, const char *def) {
    const char *path = getenv(envname1);
    if (path == nullptr)  /* no environment variable? */
        path = getenv(envname2);  /* try alternative name */
    if (path == nullptr || noenv(L))  /* no environment variable? */
        lua_pushstring(L, def);  /* use default */
    else {
        /* replace ";;" by ";AUXMARK;" and then AUXMARK by default path */
        path = luaL_gsub(L, path, LUA_PATH_SEP LUA_PATH_SEP,
            LUA_PATH_SEP AUXMARK LUA_PATH_SEP);
        luaL_gsub(L, path, AUXMARK, def);
        lua_remove(L, -2);
    }
    setprogdir(L);
    lua_setfield(L, -2, fieldname);
}


static const luaL_Reg pk_funcs[] = {
    //{"loadlib", ll_loadlib},
    //{"searchpath", ll_searchpath},
#if defined(LUA_COMPAT_MODULE)
  { "seeall", ll_seeall },
#endif
  /* placeholders */
  { "preload", nullptr },
  //{"cpath", nullptr},
  { "path", nullptr },
  { "searchers", nullptr },
  // {"uvm_searchers", nullptr},
  { "loaded", nullptr },
  { nullptr, nullptr }
};


static const luaL_Reg ll_funcs[] = {
#if defined(LUA_COMPAT_MODULE)
  { "module", ll_module },
#endif
  { "require", ll_require },
  { "import_contract", ll_import_contract },
  { "import_contract_from_address", ll_import_contract_from_address },
  { nullptr, nullptr }
};


static void createsearcherstable(lua_State *L) {
    static const lua_CFunction searchers[] =
    { searcher_preload, searcher_uvm, nullptr }; // searcher_Lua searcher_preload

    static const lua_CFunction uvm_searchers[] =
    { searcher_preload, searcher_uvm };
	UNUSED(uvm_searchers);

    int i;
    /* create 'searchers' table */
    lua_createtable(L, sizeof(searchers) / sizeof(searchers[0]) - 1, 0);

    /* fill it with predefined searchers */
    for (i = 0; searchers[i] != nullptr; i++) {
        lua_pushvalue(L, -2);  /* set 'package' as upvalue for all searchers */
        lua_pushcclosure(L, searchers[i], 1);
        lua_rawseti(L, -2, i + 1);
    }
#if defined(LUA_COMPAT_LOADERS)
    lua_pushvalue(L, -1);  /* make a copy of 'searchers' table */
    lua_setfield(L, -3, "loaders");  /* put it in field 'loaders' */
#endif
    lua_setfield(L, -2, "searchers");  /* put it in field 'searchers' */

    /*
    lua_createtable(L, sizeof(uvm_searchers) / sizeof(uvm_searchers[0]) - 1, 0);

    for (i = 0; uvm_searchers[i] != nullptr; i++) {
    lua_pushvalue(L, -2);
    lua_pushcclosure(L, uvm_searchers[i], 1);
    lua_rawseti(L, -2, i + 1);
    }
    */
    // lua_setfield(L, -4, "uvm_searchers");
}


/*
** create table CLIBS to keep track of loaded C libraries,
** setting a finalizer to close all libraries when closing state.
*/
static void createclibstable(lua_State *L) {
    lua_newtable(L);  /* create CLIBS table */
    lua_createtable(L, 0, 1);  /* create metatable for CLIBS */
    lua_pushcfunction(L, gctm);
    lua_setfield(L, -2, "__gc");  /* set finalizer for CLIBS table */
    lua_setmetatable(L, -2);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &CLIBS);  /* set CLIBS table in registry */
}


LUAMOD_API int luaopen_package(lua_State *L) {
    createclibstable(L);
    luaL_newlib(L, pk_funcs);  /* create 'package' table */
    createsearcherstable(L);
    /* set field 'path' */
    setpath(L, "path", LUA_PATHVARVERSION, LUA_PATH_VAR, LUA_PATH_DEFAULT);
    /* set field 'cpath' */
    setpath(L, "cpath", LUA_CPATHVARVERSION, LUA_CPATH_VAR, LUA_CPATH_DEFAULT); // READ: first search preload, then lua, then c lib, then others(all in one loader, not seen yet)
    /* store config information */
    lua_pushliteral(L, LUA_DIRSEP "\n" LUA_PATH_SEP "\n" LUA_PATH_MARK "\n"
        LUA_EXEC_DIR "\n" LUA_IGMARK "\n");
    lua_setfield(L, -2, "config");
    /* set field 'loaded' */
    luaL_getsubtable(L, LUA_REGISTRYINDEX, "_LOADED");
    lua_setfield(L, -2, "loaded");
    /* set field 'preload' */
    luaL_getsubtable(L, LUA_REGISTRYINDEX, "_PRELOAD");
    lua_setfield(L, -2, "preload");
    lua_pushglobaltable(L);
    lua_pushvalue(L, -2);  /* set 'package' as upvalue for next lib */
    luaL_setfuncs(L, ll_funcs, 1);  /* open lib into global table */
    lua_pop(L, 1);  /* pop global table */
    return 1;  /* return 'package' table */
}

