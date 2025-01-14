/*
** $Id: lmem.c,v 1.91 2015/03/06 19:45:54 roberto Exp $
** Interface to Memory Manager
** See Copyright Notice in lua.h
*/

#define lmem_cpp
#define LUA_CORE

#include "uvm/lprefix.h"


#include <stddef.h>

#include "uvm/lua.h"

#include "uvm/ldebug.h"
#include "uvm/ldo.h"
#include "uvm/lgc.h"
#include "uvm/lmem.h"
#include "uvm/lobject.h"
#include "uvm/lstate.h"



/*
** About the realloc function:
** void * frealloc (void *ud, void *ptr, size_t osize, size_t nsize);
** ('osize' is the old size, 'nsize' is the new size)
**
** * frealloc(ud, nullptr, x, s) creates a new block of size 's' (no
** matter 'x').
**
** * frealloc(ud, p, x, 0) frees the block 'p'
** (in this specific case, frealloc must return nullptr);
** particularly, frealloc(ud, nullptr, 0, 0) does nothing
** (which is equivalent to free(nullptr) in ISO C)
**
** frealloc returns nullptr if it cannot create or reallocate the area
** (any reallocation to an equal or smaller size cannot fail!)
*/



#define MINSIZEARRAY	4


void *luaM_growaux_(lua_State *L, void *block, int *size, size_t size_elems,
    int limit, const char *what) {
    void *newblock;
    int newsize;
    if (*size >= limit / 2) {  /* cannot double it? */
        if (*size >= limit)  /* cannot grow even a little? */
            luaG_runerror(L, "too many %s (limit is %d)", what, limit);
        newsize = limit;  /* still have at least one free place */
    }
    else {
        newsize = (*size) * 2;
        if (newsize < MINSIZEARRAY)
            newsize = MINSIZEARRAY;  /* minimum size */
    }
    newblock = luaM_reallocv(L, block, *size, newsize, size_elems);
    *size = newsize;  /* update only when everything else is OK */
    return newblock;
}


l_noret luaM_toobig(lua_State *L) {
    luaG_runerror(L, "memory allocation error: block too big");
}



/*
** generic allocation routine.
*/
void *luaM_realloc_(lua_State *L, void *block, size_t osize, size_t nsize) {
    void *newblock;
    size_t realosize = (block) ? osize : 0;
	UNUSED(realosize);
    lua_assert((realosize == 0) == (block == nullptr));
#if defined(HARDMEMTESTS)
    if (nsize > realosize && g->gcrunning)
        luaC_fullgc(L, 1);  /* force a GC whenever possible */
#endif
    newblock = (*L->frealloc)(L->ud, block, osize, nsize);
    if (newblock == nullptr && nsize > 0) {
        lua_assert(nsize > realosize);  /* cannot fail when shrinking a block */
		luaC_fullgc(L, 1);  /* try to free some memory... */
		newblock = (*L->frealloc)(L->ud, block, osize, nsize);  /* try again */
        if (newblock == nullptr)
            luaD_throw(L, LUA_ERRMEM);
    }
    lua_assert((nsize == 0) == (newblock == nullptr));
    return newblock;
}

void *luaM_realloc_inState(lua_State *L, void *block, size_t osize, size_t nsize) {
	void *newblock;
	size_t realosize = (block) ? osize : 0;
	UNUSED(realosize);
	lua_assert((realosize == 0) == (block == nullptr));
#if defined(HARDMEMTESTS)
	if (nsize > realosize && g->gcrunning)
		luaC_fullgc(L, 1);  /* force a GC whenever possible */
#endif
	newblock = lua_realloc(L, block, osize, nsize);
	if (newblock == nullptr && nsize > 0) {
		lua_assert(nsize > realosize);  /* cannot fail when shrinking a block */
		luaC_fullgc(L, 1);  /* try to free some memory... */
		newblock = lua_realloc(L, block, osize, nsize);  /* try again */
		if (newblock == nullptr)
			luaD_throw(L, LUA_ERRMEM);
	}
	lua_assert((nsize == 0) == (newblock == nullptr));
	return newblock;
}
