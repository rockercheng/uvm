/*
** $Id: lauxlib.h,v 1.129 2015/11/23 11:29:43 roberto Exp $
** Auxiliary functions for building Lua libraries
** See Copyright Notice in lua.h
*/


#ifndef lauxlib_h
#define lauxlib_h


#include <stddef.h>
#include <stdio.h>
#include <list>

#include "uvm/lua.h"
#include "uvm/uvm_api.h"
#include <cborcpp/cbor.h>

#include "lobject.h"
//#include <fc/io/json.hpp>
#include <fc/variant_object.hpp>



/* extra error code for 'luaL_load' */
#define LUA_ERRFILE     (LUA_ERRERR+1)


typedef struct luaL_Reg {
    const char *name;
    lua_CFunction func;
} luaL_Reg;


#define LUAL_NUMSIZES	(sizeof(lua_Integer)*16 + sizeof(lua_Number))

LUALIB_API void (luaL_checkversion_)(lua_State *L, lua_Number ver, size_t sz);
#define luaL_checkversion(L)  \
	  luaL_checkversion_(L, LUA_VERSION_NUM, LUAL_NUMSIZES)

LUALIB_API int (luaL_getmetafield)(lua_State *L, int obj, const char *e);
LUALIB_API int (luaL_callmeta)(lua_State *L, int obj, const char *e);
LUALIB_API const char *(luaL_tolstring)(lua_State *L, int idx, size_t *len);

typedef struct LuaCompileFilePreloadResult
{
    const char *chunk_name;
} LuaCompileFilePreloadResult;

#define LUA_MAP_TRAVERSER_MAX_DEPTH 100

UvmTableMapP(lua_table_to_map)(lua_State *L, int index);
UvmTableMapP(lua_table_to_map_with_nested)(lua_State *L, int index, std::list<const void*> &jsons, size_t recur_depth);
struct UvmStorageValue(lua_type_to_storage_value_type)(lua_State *L, int index, size_t len);
struct UvmStorageValue(lua_type_to_storage_value_type_with_nested)(lua_State *L, int index, size_t len, std::list<const void*> &jsons, size_t recur_depth);
bool (lua_table_to_map_traverser)(lua_State *L, void *ud);
bool (lua_table_to_map_traverser_with_nested)(lua_State *L, void *ud, size_t len, std::list<const void*> &jsons, size_t recur_depth);
LUALIB_API const char *(luaL_tojsonstring)(lua_State *L, int idx, size_t *len);
LUALIB_API cbor::CborObjectP(luaL_to_cbor)(lua_State* L, int idx);
LUALIB_API int (luaL_push_cbor_as_json)(lua_State* L, cbor::CborObjectP cbor_object);

LUALIB_API int (luaL_argerror)(lua_State *L, int arg, const char *extramsg);
LUALIB_API const char *(luaL_checklstring)(lua_State *L, int arg,
    size_t *l);
LUALIB_API const char *(luaL_optlstring)(lua_State *L, int arg,
    const char *def, size_t *l);
LUALIB_API lua_Number(luaL_checknumber) (lua_State *L, int arg);
LUALIB_API lua_Number(luaL_optnumber) (lua_State *L, int arg, lua_Number def);

LUALIB_API lua_Integer(luaL_checkinteger) (lua_State *L, int arg);
LUALIB_API lua_Integer(luaL_optinteger) (lua_State *L, int arg,
    lua_Integer def);

LUALIB_API void (luaL_checkstack)(lua_State *L, int sz, const char *msg);
LUALIB_API void (luaL_checktype)(lua_State *L, int arg, int t);
LUALIB_API void (luaL_checkany)(lua_State *L, int arg);

LUALIB_API int   (luaL_newmetatable)(lua_State *L, const char *tname);
LUALIB_API void  (luaL_setmetatable)(lua_State *L, const char *tname);
LUALIB_API void *(luaL_testudata)(lua_State *L, int ud, const char *tname);
LUALIB_API void *(luaL_checkudata)(lua_State *L, int ud, const char *tname);

LUALIB_API void (luaL_where)(lua_State *L, int lvl);
LUALIB_API int (luaL_error)(lua_State *L, const char *fmt, ...);

LUALIB_API int (luaL_checkoption)(lua_State *L, int arg, const char *def,
    const char *const lst[]);

LUALIB_API int (luaL_fileresult)(lua_State *L, int stat, const char *fname);
LUALIB_API int (luaL_execresult)(lua_State *L, int stat);

/* predefined references */
#define LUA_NOREF       (-2)
#define LUA_REFNIL      (-1)

LUALIB_API int (luaL_ref)(lua_State *L, int t);
LUALIB_API void (luaL_unref)(lua_State *L, int t, int ref);

LUALIB_API bool luaL_is_bytecode_file(lua_State *L, const char *filename);

LUALIB_API int (luaL_loadfilex)(lua_State *L, const char *filename,
    const char *mode);

#define luaL_loadfile(L,f)	luaL_loadfilex(L,f,nullptr)

LUALIB_API int (luaL_loadbufferx)(lua_State *L, const char *buff, size_t sz,
    const char *name, const char *mode);

LUALIB_API int(luaL_loadbufferx_with_check)(lua_State *L, const char *buff, size_t sz,
    const char *name, const char *mode, const int check_type);

LUALIB_API int (luaL_loadstring)(lua_State *L, const char *s);

LUALIB_API lua_State *(luaL_newstate)(void);

LUALIB_API lua_Integer(luaL_len) (lua_State *L, int idx);

LUALIB_API const char *(luaL_gsub)(lua_State *L, const char *s, const char *p,
    const char *r);

LUALIB_API void (luaL_setfuncs)(lua_State *L, const luaL_Reg *l, int nup);

LUALIB_API int (luaL_getsubtable)(lua_State *L, int idx, const char *fname);

LUALIB_API void (luaL_traceback)(lua_State *L, lua_State *L1,
    const char *msg, int level);

LUALIB_API void (luaL_requiref)(lua_State *L, const char *modname,
    lua_CFunction openf, int glb);

/**
 * before call this, push module filename to stack
 */
int luaL_require_module(lua_State *L);

LUA_API int lua_execute_contract_api_by_stream(lua_State *L, UvmModuleByteStream *stream,
	const char *api_name, cbor::CborArrayValue& args, std::string *result_json_string);

size_t luaL_wrap_contract_apis(lua_State *L, int index, void *ud);

/**
 * get contract apis in stream
 */
bool luaL_get_contract_apis(lua_State *L, UvmModuleByteStream *stream, char *error = nullptr);

int luaL_import_contract_module(lua_State *L);

int luaL_import_contract_module_from_address(lua_State *L);

std::shared_ptr<UvmModuleByteStream> lua_common_open_contract(lua_State *L, const char *name, char *error = nullptr);

UvmTableMapP luaL_create_lua_table_map_in_memory_pool(lua_State *L);

namespace fc {
	void to_variant(std::map<std::string, TValue> m, variant& vo);
}




/************************************************************************/
/* init storage, name, id, and some other info to contract, stack top */
/************************************************************************/
void lua_fill_contract_info_for_use(lua_State *L);

/*
** ===============================================================
** some useful macros
** ===============================================================
*/


#define luaL_newlibtable(L,l)	\
  lua_createtable(L, 0, sizeof(l)/sizeof((l)[0]) - 1)

#define luaL_newlib(L,l)  \
  (luaL_checkversion(L), luaL_newlibtable(L,l), luaL_setfuncs(L,l,0))

#define luaL_argcheck(L, cond,arg,extramsg)	\
		((void)((cond) || luaL_argerror(L, (arg), (extramsg))))
#define luaL_checkstring(L,n)	(luaL_checklstring(L, (n), nullptr))
#define luaL_optstring(L,n,d)	(luaL_optlstring(L, (n), (d), nullptr))

#define luaL_typename(L,i)	lua_typename(L, lua_type(L,(i)))

#define luaL_dofile(L, fn) \
	(luaL_loadfile(L, fn) || lua_pcall(L, 0, LUA_MULTRET, 0))

#define luaL_dostring(L, s) \
	(luaL_loadstring(L, s) || lua_pcall(L, 0, LUA_MULTRET, 0))

#define luaL_getmetatable(L,n)	(lua_getfield(L, LUA_REGISTRYINDEX, (n)))

#define luaL_opt(L,f,n,d)	(lua_isnoneornil(L,(n)) ? (d) : f(L,(n)))

#define luaL_loadbuffer(L,s,sz,n)	luaL_loadbufferx(L,s,sz,n,nullptr)


/*
** {======================================================
** Generic Buffer manipulation
** =======================================================
*/

typedef struct luaL_Buffer {
    char *b;  /* buffer address */
    size_t size;  /* buffer size */
    size_t n;  /* number of characters in buffer */
    lua_State *L;
    char initb[LUAL_BUFFERSIZE];  /* initial buffer */
} luaL_Buffer;


#define luaL_addchar(B,c) \
  ((void)((B)->n < (B)->size || luaL_prepbuffsize((B), 1)), \
   ((B)->b[(B)->n++] = (c)))

#define luaL_addsize(B,s)	((B)->n += (s))

LUALIB_API void (luaL_buffinit)(lua_State *L, luaL_Buffer *B);
LUALIB_API char *(luaL_prepbuffsize)(luaL_Buffer *B, size_t sz);
LUALIB_API void (luaL_addlstring)(luaL_Buffer *B, const char *s, size_t l);
LUALIB_API void (luaL_addstring)(luaL_Buffer *B, const char *s);
LUALIB_API void (luaL_addvalue)(luaL_Buffer *B);
LUALIB_API void (luaL_pushresult)(luaL_Buffer *B);
LUALIB_API void (luaL_pushresultsize)(luaL_Buffer *B, size_t sz);
LUALIB_API char *(luaL_buffinitsize)(lua_State *L, luaL_Buffer *B, size_t sz);

#define luaL_prepbuffer(B)	luaL_prepbuffsize(B, LUAL_BUFFERSIZE)

/* }====================================================== */



/*
** {======================================================
** File handles for IO library
** =======================================================
*/

/*
** A file handle is a userdata with metatable 'LUA_FILEHANDLE' and
** initial structure 'luaL_Stream' (it may contain other fields
** after that initial structure).
*/

#define LUA_FILEHANDLE          "FILE*"


typedef struct luaL_Stream {
    FILE *f;  /* stream (nullptr for incompletely created streams) */
    lua_CFunction closef;  /* to close stream (nullptr for closed streams) */
} luaL_Stream;

/* }====================================================== */



/* compatibility with old module system */
#if defined(LUA_COMPAT_MODULE)

LUALIB_API void (luaL_pushmodule)(lua_State *L, const char *modname,
    int sizehint);
LUALIB_API void (luaL_openlib)(lua_State *L, const char *libname,
    const luaL_Reg *l, int nup);

#define luaL_register(L,n,l)	(luaL_openlib(L,(n),(l),0))

#endif


/*
** {==================================================================
** "Abstraction Layer" for basic report of messages and errors
** ===================================================================
*/

/* print a string */
#if !defined(lua_writestring)
#define lua_writestring(s,l)   do { fwrite((s), sizeof(char), (l), stdout); } while(0)
#endif

#if !defined(luaL_writestring)
#define luaL_writestring(L,s,l)    do { if((L)->out) fwrite((s), sizeof(char), (l), (L)->out); } while(0)
#endif

/* print a newline and flush the output */
#if !defined(lua_writeline)
#define lua_writeline()        do { lua_writestring("\n", 1); fflush(stdout); } while(0)
#endif

#if !defined(luaL_writeline)
#define luaL_writeline(L)        do { if((L)->out) { luaL_writestring((L), "\n", 1); fflush((L)->out); } } while(0)
#endif

/* print an error message */
#if !defined(lua_writestringerror)
#define lua_writestringerror(s,p) \
        (fprintf(stderr, (s), (p)), fflush(stderr))
#endif

#if !defined(luaL_writestringerror)
#define luaL_writestringerror(L,s,p) \
        do { if((L)->err) (fprintf((L)->err, (s), (p)), fflush((L)->err)); } while(0)
#endif

/* }================================================================== */


/*
** {============================================================
** Compatibility with deprecated conversions
** =============================================================
*/
#if defined(LUA_COMPAT_APIINTCASTS)

#define luaL_checkunsigned(L,a)	((lua_Unsigned)luaL_checkinteger(L,a))
#define luaL_optunsigned(L,a,d)	\
	((lua_Unsigned)luaL_optinteger(L,a,(lua_Integer)(d)))

#define luaL_checkint(L,n)	((int)luaL_checkinteger(L, (n)))
#define luaL_optint(L,n,d)	((int)luaL_optinteger(L, (n), (d)))

#define luaL_checklong(L,n)	((long)luaL_checkinteger(L, (n)))
#define luaL_optlong(L,n,d)	((long)luaL_optinteger(L, (n), (d)))

#endif
/* }============================================================ */



#endif


