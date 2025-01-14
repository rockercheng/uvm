/*
** $Id: lapi.c,v 2.257 2015/11/02 18:48:07 roberto Exp $
** Lua API
** See Copyright Notice in lua.h
*/

#define lapi_cpp
#define LUA_CORE

#include <uvm/lprefix.h>


#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdio.h>
#include <algorithm>

#include "uvm/lua.h"

#include "uvm/lapi.h"
#include "uvm/ldebug.h"
#include "uvm/ldo.h"
#include "uvm/lfunc.h"
#include "uvm/lgc.h"
#include "uvm/lmem.h"
#include "uvm/lobject.h"
#include "uvm/lstate.h"
#include "uvm/lstring.h"
#include "uvm/ltable.h"
#include "uvm/ltm.h"
#include "uvm/lundump.h"
#include "uvm/lvm.h"
#include "uvm/ldebug.h"
#include "uvm/lopcodes.h"

const char lua_ident[] =
"$LuaVersion: " LUA_COPYRIGHT " $"
"$LuaAuthors: " LUA_AUTHORS " $";


/* value at a non-valid index */
#define NONVALIDVALUE		lua_cast(TValue *, luaO_nilobject)

/* corresponding test */
#define isvalid(o)	((o) != luaO_nilobject)

/* test for pseudo index */
#define ispseudo(i)		((i) <= LUA_REGISTRYINDEX)

/* test for upvalue */
#define isupvalue(i)		((i) < LUA_REGISTRYINDEX)

/* test for valid but not pseudo index */
#define isstackindex(i, o)	(isvalid(o) && !ispseudo(i))

#define api_checkvalidindex(l,o)  api_check(l, isvalid(o), "invalid index")

#define api_checkstackindex(l, i, o)  \
	api_check(l, isstackindex(i, o), "index not in the stack")


static TValue *index2addr(lua_State *L, int idx) {
    CallInfo *ci = L->ci;
    if (idx > 0) {
        TValue *o = ci->func + idx;
        api_check(L, idx <= ci->top - (ci->func + 1), "unacceptable index");
        if (o >= L->top) return NONVALIDVALUE;
        else return o;
    }
    else if (!ispseudo(idx)) {  /* negative index */
        api_check(L, idx != 0 && -idx <= L->top - (ci->func + 1), "invalid index");
        return L->top + idx;
    }
    else if (idx == LUA_REGISTRYINDEX)
        return &L->l_registry;
    else {  /* upvalues */
        idx = LUA_REGISTRYINDEX - idx;
        api_check(L, idx <= MAXUPVAL + 1, "upvalue index too large");
        if (ttislcf(ci->func))  /* light C function? */
            return NONVALIDVALUE;  /* it has no upvalues */
        else {
            uvm_types::GcCClosure *func = clCvalue(ci->func);
            return (idx <= func->nupvalues) ? &func->upvalue[idx - 1] : NONVALIDVALUE;
        }
    }
}


/*
** to be called by 'lua_checkstack' in protected mode, to grow stack
** capturing memory errors
*/
static void growstack(lua_State *L, void *ud) {
    int size = *(int *)ud;
    luaD_growstack(L, size);
}


LUA_API int lua_checkstack(lua_State *L, int n) {
    int res;
    CallInfo *ci = L->ci;
    lua_lock(L);
    api_check(L, n >= 0, "negative 'n'");
    if (L->stack_last - L->top > n)  /* stack large enough? */
        res = 1;  /* yes; check is OK */
    else {  /* no; need to grow stack */
        int inuse = cast_int(L->top - L->stack) + EXTRA_STACK;
        if (inuse > LUAI_MAXSTACK - n)  /* can grow without overflow? */
            res = 0;  /* no */
        else  /* try to grow stack */
            res = (luaD_rawrunprotected(L, &growstack, &n) == LUA_OK);
    }
    if (res && ci->top < L->top + n)
        ci->top = L->top + n;  /* adjust frame top */
    lua_unlock(L);
    return res;
}


LUA_API void lua_xmove(lua_State *from, lua_State *to, int n) {
    int i;
    if (from == to) return;
    lua_lock(to);
    api_checknelems(from, n);
    api_check(from, to->ci->top - to->top >= n, "stack overflow");
    from->top -= n;
    for (i = 0; i < n; i++) {
        setobj2s(to, to->top, from->top + i);
        to->top++;  /* stack already checked by previous 'api_check' */
    }
    lua_unlock(to);
}


LUA_API lua_CFunction lua_atpanic(lua_State *L, lua_CFunction panicf) {
    lua_CFunction old;
    lua_lock(L);
    old = L->panic;
	L->panic = panicf;
    lua_unlock(L);
    return old;
}


LUA_API const lua_Number *lua_version(lua_State *L) {
    static const lua_Number version = LUA_VERSION_NUM;
    if (L == nullptr) return &version;
    else return L->version;
}



/*
** basic stack manipulation
*/


/*
** convert an acceptable stack index into an absolute index
*/
LUA_API int lua_absindex(lua_State *L, int idx) {
    return (idx > 0 || ispseudo(idx))
        ? idx
        : cast_int(L->top - L->ci->func) + idx;
}


LUA_API int lua_gettop(lua_State *L) {
    return cast_int(L->top - (L->ci->func + 1));
}


LUA_API void lua_settop(lua_State *L, int idx) {
    StkId func = L->ci->func;
    lua_lock(L);
    if (idx >= 0) {
        api_check(L, idx <= L->stack_last - (func + 1), "new top too large");
        while (L->top < (func + 1) + idx)
            setnilvalue(L->top++);
        L->top = (func + 1) + idx;
    }
    else {
        api_check(L, -(idx + 1) <= (L->top - (func + 1)), "invalid new top");
        L->top += idx + 1;  /* 'subtract' index (index is negative) */
    }
    lua_unlock(L);
}


/*
** Reverse the stack segment from 'from' to 'to'
** (auxiliary to 'lua_rotate')
*/
static void reverse(lua_State *L, StkId from, StkId to) {
    for (; from < to; from++, to--) {
        TValue temp;
        setobj(L, &temp, from);
        setobjs2s(L, from, to);
        setobj2s(L, to, &temp);
    }
}


/*
** Let x = AB, where A is a prefix of length 'n'. Then,
** rotate x n == BA. But BA == (A^r . B^r)^r.
*/
LUA_API void lua_rotate(lua_State *L, int idx, int n) {
    StkId p, t, m;
    lua_lock(L);
    t = L->top - 1;  /* end of stack segment being rotated */
    p = index2addr(L, idx);  /* start of segment */
    api_checkstackindex(L, idx, p);
    api_check(L, (n >= 0 ? n : -n) <= (t - p + 1), "invalid 'n'");
    m = (n >= 0 ? t - n : p - n - 1);  /* end of prefix */
    reverse(L, p, m);  /* reverse the prefix with length 'n' */
    reverse(L, m + 1, t);  /* reverse the suffix */
    reverse(L, p, t);  /* reverse the entire segment */
    lua_unlock(L);
}


LUA_API void lua_copy(lua_State *L, int fromidx, int toidx) {
    TValue *fr, *to;
    lua_lock(L);
    fr = index2addr(L, fromidx);
    to = index2addr(L, toidx);
    api_checkvalidindex(L, to);
    setobj(L, to, fr);
    /* LUA_REGISTRYINDEX does not need gc barrier
       (collector revisits it before finishing collection) */
    lua_unlock(L);
}


LUA_API void lua_pushvalue(lua_State *L, int idx) {
    lua_lock(L);
    setobj2s(L, L->top, index2addr(L, idx));
    api_incr_top(L);
    lua_unlock(L);
}



/*
** access functions (stack -> C)
*/


LUA_API int lua_type(lua_State *L, int idx) {
    StkId o = index2addr(L, idx);
    return (isvalid(o) ? ttnov(o) : LUA_TNONE);
}


LUA_API const char *lua_typename(lua_State *L, int t) {
    UNUSED(L);
    api_check(L, LUA_TNONE <= t && t < LUA_NUMTAGS, "invalid tag");
    return ttypename(t);
}


LUA_API int lua_iscfunction(lua_State *L, int idx) {
    StkId o = index2addr(L, idx);
    return (ttislcf(o) || (ttisCclosure(o)));
}


LUA_API int lua_isinteger(lua_State *L, int idx) {
    StkId o = index2addr(L, idx);
    return ttisinteger(o);
}


LUA_API int lua_isnumber(lua_State *L, int idx) {
    lua_Number n;
    const TValue *o = index2addr(L, idx);
    return tonumber(o, &n);
}


LUA_API int lua_isstring(lua_State *L, int idx) {
    const TValue *o = index2addr(L, idx);
    return (ttisstring(o) || cvt2str(o));
}


LUA_API int lua_isuserdata(lua_State *L, int idx) {
    const TValue *o = index2addr(L, idx);
    return (ttisfulluserdata(o) || ttislightuserdata(o));
}


LUA_API int lua_rawequal(lua_State *L, int index1, int index2) {
    StkId o1 = index2addr(L, index1);
    StkId o2 = index2addr(L, index2);
    return (isvalid(o1) && isvalid(o2)) ? luaV_rawequalobj(o1, o2) : 0;
}


LUA_API void lua_arith(lua_State *L, int op) {
    lua_lock(L);
    if (op != LUA_OPUNM && op != LUA_OPBNOT)
        api_checknelems(L, 2);  /* all other operations expect two operands */
    else {  /* for unary operations, add fake 2nd operand */
        api_checknelems(L, 1);
        setobjs2s(L, L->top, L->top - 1);
        api_incr_top(L);
    }
    /* first operand at top - 2, second at top - 1; result go to top - 2 */
    luaO_arith(L, op, L->top - 2, L->top - 1, L->top - 2);
    L->top--;  /* remove second operand */
    lua_unlock(L);
}


LUA_API int lua_compare(lua_State *L, int index1, int index2, int op) {
    StkId o1, o2;
    int i = 0;
    lua_lock(L);  /* may call tag method */
    o1 = index2addr(L, index1);
    o2 = index2addr(L, index2);
    if (isvalid(o1) && isvalid(o2)) {
        switch (op) {
        case LUA_OPEQ: i = luaV_equalobj(L, o1, o2); break;
        case LUA_OPLT: i = luaV_lessthan(L, o1, o2); break;
        case LUA_OPLE: i = luaV_lessequal(L, o1, o2); break;
        default: api_check(L, 0, "invalid option");
        }
    }
    lua_unlock(L);
    return i;
}


LUA_API size_t lua_stringtonumber(lua_State *L, const char *s) {
    size_t sz = luaO_str2num(s, L->top);
    if (sz != 0)
        api_incr_top(L);
    return sz;
}


LUA_API lua_Number lua_tonumberx(lua_State *L, int idx, int *pisnum) {
    lua_Number n;
    const TValue *o = index2addr(L, idx);
    int isnum = tonumber(o, &n);
    if (!isnum)
        n = 0;  /* call to 'tonumber' may change 'n' even if it fails */
    if (pisnum) *pisnum = isnum;
    return n;
}


LUA_API lua_Integer lua_tointegerx(lua_State *L, int idx, int *pisnum) {
    lua_Integer res;
    const TValue *o = index2addr(L, idx);
    int isnum = tointeger(o, &res);
    if (!isnum)
        res = 0;  /* call to 'tointeger' may change 'n' even if it fails */
    if (pisnum) *pisnum = isnum;
    return res;
}


LUA_API int lua_toboolean(lua_State *L, int idx) {
    const TValue *o = index2addr(L, idx);
    return !l_isfalse(o);
}


LUA_API const char *lua_tolstring(lua_State *L, int idx, size_t *len) {
    StkId o = index2addr(L, idx);
    if (!ttisstring(o)) {
        if (!cvt2str(o)) {  /* not convertible? */
            if (len != nullptr) *len = 0;
            return nullptr;
        }
        lua_lock(L);  /* 'luaO_tostring' may create a new string */
        luaC_checkGC(L);
        o = index2addr(L, idx);  /* previous call may reallocate the stack */
        luaO_tostring(L, o);
        lua_unlock(L);
    }
    if (len != nullptr)
        *len = vslen(o);
    return svalue(o);
}


LUA_API size_t lua_rawlen(lua_State *L, int idx) {
    StkId o = index2addr(L, idx);
    switch (ttype(o)) {
    case LUA_TSHRSTR: return tsvalue(o)->value.size();
    case LUA_TLNGSTR: return tsvalue(o)->value.size();
    case LUA_TUSERDATA: return uvalue(o)->len;
    case LUA_TTABLE: return luaH_getn(hvalue(o));
    default: return 0;
    }
}


LUA_API lua_CFunction lua_tocfunction(lua_State *L, int idx) {
    StkId o = index2addr(L, idx);
    if (ttislcf(o)) return fvalue(o);
    else if (ttisCclosure(o))
        return clCvalue(o)->f;
    else return nullptr;  /* not a C function */
}


LUA_API void *lua_touserdata(lua_State *L, int idx) {
    StkId o = index2addr(L, idx);
    switch (ttnov(o)) {
    case LUA_TUSERDATA: return getudatamem(uvalue(o));
    case LUA_TLIGHTUSERDATA: return pvalue(o);
    default: return nullptr;
    }
}


LUA_API lua_State *lua_tothread(lua_State *L, int idx) {
    StkId o = index2addr(L, idx);
    return (!ttisthread(o)) ? nullptr : thvalue(o);
}


LUA_API const void *lua_topointer(lua_State *L, int idx) {
    StkId o = index2addr(L, idx);
    switch (ttype(o)) {
    case LUA_TTABLE: return hvalue(o);
    case LUA_TLCL: return clLvalue(o);
    case LUA_TCCL: return clCvalue(o);
    case LUA_TLCF: return lua_cast(void *, lua_cast(size_t, fvalue(o)));
    case LUA_TTHREAD: return thvalue(o);
    case LUA_TUSERDATA: return getudatamem(uvalue(o));
    case LUA_TLIGHTUSERDATA: return pvalue(o);
    default: return nullptr;
    }
}

LUA_API void lua_settableonlyread(lua_State *L, int idx, bool isOnlyRead) {
	StkId o = index2addr(L, idx);
	if (ttype(o) == LUA_TTABLE) {
		luaH_setisonlyread(L, hvalue(o), isOnlyRead);
	}
}



/*
** push functions (C -> stack)
*/


LUA_API void lua_pushnil(lua_State *L) {
    lua_lock(L);
    setnilvalue(L->top);
    api_incr_top(L);
    lua_unlock(L);
}


LUA_API void lua_pushnumber(lua_State *L, lua_Number n) {
    lua_lock(L);
    setfltvalue(L->top, n);
    api_incr_top(L);
    lua_unlock(L);
}


LUA_API void lua_pushinteger(lua_State *L, lua_Integer n) {
    lua_lock(L);
    setivalue(L->top, n);
    api_incr_top(L);
    lua_unlock(L);
}


/*
** Pushes on the stack a string with given length. Avoid using 's' when
** 'len' == 0 (as 's' can be nullptr in that case), due to later use of
** 'memcmp' and 'memcpy'.
*/
LUA_API const char *lua_pushlstring(lua_State *L, const char *s, size_t len) {
    uvm_types::GcString *ts;
    lua_lock(L);
    luaC_checkGC(L);
    ts = (len == 0) ? luaS_new(L, "") : luaS_newlstr(L, s, len);
    setsvalue2s(L, L->top, ts);
    api_incr_top(L);
    lua_unlock(L);
    return getstr(ts);
}


LUA_API const char *lua_pushstring(lua_State *L, const char *s) {
    lua_lock(L);
    if (s == nullptr)
        setnilvalue(L->top);
    else {
		uvm_types::GcString *ts;
        luaC_checkGC(L);
        ts = luaS_new(L, s);
        setsvalue2s(L, L->top, ts);
        s = getstr(ts);  /* internal copy's address */
    }
    api_incr_top(L);
    lua_unlock(L);
    return s;
}


LUA_API const char *lua_pushvfstring(lua_State *L, const char *fmt,
    va_list argp) {
    const char *ret;
    lua_lock(L);
    luaC_checkGC(L);
    ret = luaO_pushvfstring(L, fmt, argp);
    lua_unlock(L);
    return ret;
}


LUA_API const char *lua_pushfstring(lua_State *L, const char *fmt, ...) {
    const char *ret;
    va_list argp;
    lua_lock(L);
    luaC_checkGC(L);
    va_start(argp, fmt);
    ret = luaO_pushvfstring(L, fmt, argp);
    va_end(argp);
    lua_unlock(L);
    return ret;
}


LUA_API void lua_pushcclosure(lua_State *L, lua_CFunction fn, int n) {
    lua_lock(L);
    if (n == 0) {
        setfvalue(L->top, fn);
    }
    else {
        uvm_types::GcCClosure *cl;
        api_checknelems(L, n);
        api_check(L, n <= MAXUPVAL, "upvalue index too large");
        luaC_checkGC(L);
        cl = luaF_newCclosure(L, n);
        cl->f = fn;
        L->top -= n;
        while (n--) {
            setobj2n(L, &cl->upvalue[n], L->top + n);
            /* does not need barrier because closure is white */
        }
        setclCvalue(L, L->top, cl);
    }
    api_incr_top(L);
    lua_unlock(L);
}


LUA_API void lua_pushboolean(lua_State *L, int b) {
    lua_lock(L);
    setbvalue(L->top, (b != 0));  /* ensure that true is 1 */
    api_incr_top(L);
    lua_unlock(L);
}


LUA_API void lua_pushlightuserdata(lua_State *L, void *p) {
    lua_lock(L);
    setpvalue(L->top, p);
    api_incr_top(L);
    lua_unlock(L);
}


LUA_API int lua_pushthread(lua_State *L) {
    lua_lock(L);
    setthvalue(L, L->top, L);
    api_incr_top(L);
    lua_unlock(L);
    return true;
}



/*
** get functions (Lua -> stack)
*/


static int auxgetstr(lua_State *L, const TValue *t, const char *k) {
    const TValue *aux;
	uvm_types::GcString *str = luaS_new(L, k);
    if (luaV_fastget(L, t, str, aux, luaH_getstr)) {
        setobj2s(L, L->top, aux);
        api_incr_top(L);
    }
    else {
        setsvalue2s(L, L->top, str);
        api_incr_top(L);
        luaV_finishget(nullptr, L, t, L->top - 1, L->top - 1, aux); // TODO: ctx
    }
    lua_unlock(L);
    return ttnov(L->top - 1);
}


LUA_API int lua_getglobal(lua_State *L, const char *name) {
    auto *reg = hvalue(&L->l_registry);
    lua_lock(L);
    return auxgetstr(L, luaH_getint(reg, LUA_RIDX_GLOBALS), name);
}


LUA_API int lua_gettable(lua_State *L, int idx) {
    StkId t;
    lua_lock(L);
    t = index2addr(L, idx);
    luaV_gettable(L, t, L->top - 1, L->top - 1); // TODO: ctx
    lua_unlock(L);
    return ttnov(L->top - 1);
}


LUA_API int lua_getfield(lua_State *L, int idx, const char *k) {
    lua_lock(L);
    return auxgetstr(L, index2addr(L, idx), k);
}


LUA_API int lua_geti(lua_State *L, int idx, lua_Integer n) {
    StkId t;
    const TValue *aux;
    lua_lock(L);
    t = index2addr(L, idx);
    if (luaV_fastget(L, t, n, aux, luaH_getint)) {
        setobj2s(L, L->top, aux);
        api_incr_top(L);
    }
    else {
        setivalue(L->top, n);
        api_incr_top(L);
        luaV_finishget(nullptr, L, t, L->top - 1, L->top - 1, aux); // TODO: ctx
    }
    lua_unlock(L);
    return ttnov(L->top - 1);
}


LUA_API int lua_rawget(lua_State *L, int idx) {
    StkId t;
    lua_lock(L);
    t = index2addr(L, idx);
    api_check(L, ttistable(t), "table expected");
    setobj2s(L, L->top - 1, luaH_get(hvalue(t), L->top - 1));
    lua_unlock(L);
    return ttnov(L->top - 1);
}


LUA_API int lua_rawgeti(lua_State *L, int idx, lua_Integer n) {
    StkId t;
    lua_lock(L);
    t = index2addr(L, idx);
    api_check(L, ttistable(t), "table expected");
    setobj2s(L, L->top, luaH_getint(hvalue(t), n));
    api_incr_top(L);
    lua_unlock(L);
    return ttnov(L->top - 1);
}


LUA_API int lua_rawgetp(lua_State *L, int idx, const void *p) {
    StkId t;
    TValue k;
    lua_lock(L);
    t = index2addr(L, idx);
    api_check(L, ttistable(t), "table expected");
    setpvalue(&k, lua_cast(void *, p));
    setobj2s(L, L->top, luaH_get(hvalue(t), &k));
    api_incr_top(L);
    lua_unlock(L);
    return ttnov(L->top - 1);
}


LUA_API void lua_createtable(lua_State *L, int narray, int nrec) {
    uvm_types::GcTable *t;
    lua_lock(L);
    luaC_checkGC(L);
    t = luaH_new(L);
    sethvalue(L, L->top, t);
    api_incr_top(L);
    if (narray > 0 || nrec > 0)
        luaH_resize(L, t, narray, nrec);
    lua_unlock(L);
}


LUA_API int lua_getmetatable(lua_State *L, int objindex) {
    const TValue *obj;
	uvm_types::GcTable *mt;
    int res = 0;
    lua_lock(L);
    obj = index2addr(L, objindex);
    switch (ttnov(obj)) {
    case LUA_TTABLE:
        mt = hvalue(obj)->metatable;
        break;
    case LUA_TUSERDATA:
        mt = uvalue(obj)->metatable;
        break;
    default:
        mt = L->mt[ttnov(obj)];
        break;
    }
    if (mt != nullptr) {
        sethvalue(L, L->top, mt);
        api_incr_top(L);
        res = 1;
    }
    lua_unlock(L);
    return res;
}


LUA_API int lua_getuservalue(lua_State *L, int idx) {
    StkId o;
    lua_lock(L);
    o = index2addr(L, idx);
    api_check(L, ttisfulluserdata(o), "full userdata expected");
    getuservalue(L, uvalue(o), L->top);
    api_incr_top(L);
    lua_unlock(L);
    return ttnov(L->top - 1);
}


/*
** set functions (stack -> Lua)
*/

/*
** t[k] = value at the top of the stack (where 'k' is a string)
*/
static void auxsetstr(lua_State *L, const TValue *t, const char *k) {
	/*if (ttistable(t)) {
		auto table_addr = (intptr_t)t->value_.gco;
		if (L->allow_contract_modify != table_addr && L->contract_table_addresses
			&& std::find(L->contract_table_addresses->begin(), L->contract_table_addresses->end(), table_addr) != L->contract_table_addresses->end()) {
			auto msg = std::string("can't modify contract properties ") + k;
			luaG_runerror(L, msg.c_str());
			return;
		}
	}*/
    const TValue *aux;
	uvm_types::GcString *str = luaS_new(L, k);
    api_checknelems(L, 1);
    if (luaV_fastset(L, t, str, aux, luaH_getstr, L->top - 1))
        L->top--;  /* pop value */
    else {
        setsvalue2s(L, L->top, str);  /* push 'str' (to make it a TValue) */
        api_incr_top(L);
        luaV_finishset(nullptr, L, t, L->top - 1, L->top - 2, aux); // TODO: ctx
        L->top -= 2;  /* pop value and key */
    }
    lua_unlock(L);  /* lock done by caller */
}


LUA_API void lua_setglobal(lua_State *L, const char *name) {
	uvm_types::GcTable *reg = hvalue(&L->l_registry);
    lua_lock(L);  /* unlock done in 'auxsetstr' */
    auxsetstr(L, luaH_getint(reg, LUA_RIDX_GLOBALS), name);
}


LUA_API void lua_settable(lua_State *L, int idx) {
	/*if (lua_istable(L, idx)) {
		auto table_addr = (intptr_t)lua_topointer(L, idx);
		if (L->allow_contract_modify != table_addr && L->contract_table_addresses 
			&& std::find(L->contract_table_addresses->begin(), L->contract_table_addresses->end(), table_addr) != L->contract_table_addresses->end()) {
			luaG_runerror(L, "can't modify contract properties");
			return;
		}
	}*/
    StkId t;
    lua_lock(L);
    api_checknelems(L, 2);
    t = index2addr(L, idx);
    luaV_settable(L, t, L->top - 2, L->top - 1);
    L->top -= 2;  /* pop index and value */
    lua_unlock(L);
}


LUA_API void lua_setfield(lua_State *L, int idx, const char *k) {
	/*if (lua_istable(L, idx)) {
		auto table_addr = (intptr_t)lua_topointer(L, idx);
		if (L->allow_contract_modify != table_addr && L->contract_table_addresses
			&& std::find(L->contract_table_addresses->begin(), L->contract_table_addresses->end(), table_addr) != L->contract_table_addresses->end()) {
			auto msg = std::string("can't modify contract properties ") + k;
			luaG_runerror(L, msg.c_str());
			return;
		}
	}*/
    lua_lock(L);  /* unlock done in 'auxsetstr' */
    auxsetstr(L, index2addr(L, idx), k);
}


LUA_API void lua_seti(lua_State *L, int idx, lua_Integer n) {
    StkId t;
    const TValue *aux;
    lua_lock(L);
    api_checknelems(L, 1);
    t = index2addr(L, idx);
    if (luaV_fastset(L, t, n, aux, luaH_getint, L->top - 1))
        L->top--;  /* pop value */
    else {
        setivalue(L->top, n);
        api_incr_top(L);
        luaV_finishset(nullptr, L, t, L->top - 1, L->top - 2, aux); // TODO: ctx
        L->top -= 2;  /* pop value and key */
    }
    lua_unlock(L);
}


LUA_API void lua_rawset(lua_State *L, int idx) {
    StkId o;
    TValue *slot;
    lua_lock(L);
    api_checknelems(L, 2);
    o = index2addr(L, idx);
    api_check(L, ttistable(o), "table expected");
    slot = luaH_set(L, hvalue(o), L->top - 2, true);
    setobj2t(L, slot, L->top - 1);
	invalidateTMcache(hvalue(o));
    L->top -= 2;
    lua_unlock(L);
}


LUA_API void lua_rawseti(lua_State *L, int idx, lua_Integer n) {
    StkId o;
    lua_lock(L);
    api_checknelems(L, 1);
    o = index2addr(L, idx);
    api_check(L, ttistable(o), "table expected");
    luaH_setint(L, hvalue(o), n, L->top - 1);
    L->top--;
    lua_unlock(L);
}


LUA_API void lua_rawsetp(lua_State *L, int idx, const void *p) {
    StkId o;
    TValue k, *slot;
    lua_lock(L);
    api_checknelems(L, 1);
    o = index2addr(L, idx);
    api_check(L, ttistable(o), "table expected");
    setpvalue(&k, lua_cast(void *, p));
    slot = luaH_set(L, hvalue(o), &k, true);
    setobj2t(L, slot, L->top - 1);
    L->top--;
    lua_unlock(L);
}


LUA_API int lua_setmetatable(lua_State *L, int objindex) {
    TValue *obj;
	uvm_types::GcTable *mt;
    lua_lock(L);
    api_checknelems(L, 1);
    obj = index2addr(L, objindex);
    if (ttisnil(L->top - 1))
        mt = nullptr;
    else {
        api_check(L, ttistable(L->top - 1), "table expected");
        mt = hvalue(L->top - 1);
    }
    switch (ttnov(obj)) {
    case LUA_TTABLE: {
        hvalue(obj)->metatable = mt;
        break;
    }
    case LUA_TUSERDATA: {
        uvalue(obj)->metatable = mt;
        break;
    }
    default: {
        L->mt[ttnov(obj)] = mt;
        break;
    }
    }
    L->top--;
    lua_unlock(L);
    return 1;
}


LUA_API void lua_setuservalue(lua_State *L, int idx) {
    StkId o;
    lua_lock(L);
    api_checknelems(L, 1);
    o = index2addr(L, idx);
    api_check(L, ttisfulluserdata(o), "full userdata expected");
    setuservalue(L, uvalue(o), L->top - 1);
    L->top--;
    lua_unlock(L);
}


/*
** 'load' and 'call' functions (run Lua code)
*/


#define checkresults(L,na,nr) \
     api_check(L, (nr) == LUA_MULTRET || (L->ci->top - L->top >= (nr) - (na)), \
	"results from function overflow current stack size")


LUA_API void lua_callk(lua_State *L, int nargs, int nresults,
    lua_KContext ctx, lua_KFunction k) {
    StkId func;
    lua_lock(L);
    api_check(L, k == nullptr || !isLua(L->ci),
        "cannot use continuations inside hooks");
    api_checknelems(L, nargs + 1);
    api_check(L, L->status == LUA_OK, "cannot do calls on non-normal thread");
    checkresults(L, nargs, nresults);
    func = L->top - (nargs + 1);
    if (k != nullptr && L->nny == 0) {  /* need to prepare continuation? */
        L->ci->u.c.k = k;  /* save continuation */
        L->ci->u.c.ctx = ctx;  /* save context */
        luaD_call(L, func, nresults);  /* do the call */
    }
    else  /* no continuation or no yieldable */
        luaD_callnoyield(L, func, nresults);  /* just do the call */
    adjustresults(L, nresults);
    lua_unlock(L);
}



/*
** Execute a protected call.
*/
struct CallS {  /* data to 'f_call' */
    StkId func;
    int nresults;
};


static void f_call(lua_State *L, void *ud) {
    struct CallS *c = lua_cast(struct CallS *, ud);
    luaD_callnoyield(L, c->func, c->nresults);
}



LUA_API int lua_pcallk(lua_State *L, int nargs, int nresults, int errfunc,
    lua_KContext ctx, lua_KFunction k) {
    struct CallS c;
    int status;
    ptrdiff_t func;
    lua_lock(L);
    api_check(L, k == nullptr || !isLua(L->ci),
        "cannot use continuations inside hooks");
    api_checknelems(L, nargs + 1);
    api_check(L, L->status == LUA_OK, "cannot do calls on non-normal thread");
    checkresults(L, nargs, nresults);
    if (errfunc == 0)
        func = 0;
    else {
        StkId o = index2addr(L, errfunc);
        api_checkstackindex(L, errfunc, o);
        func = savestack(L, o);
    }
    c.func = L->top - (nargs + 1);  /* function to be called */
    if (k == nullptr || L->nny > 0) {  /* no continuation or no yieldable? */
        c.nresults = nresults;  /* do a 'conventional' protected call */
        status = luaD_pcall(L, f_call, &c, savestack(L, c.func), func);
    }
    else {  /* prepare continuation (call is already protected by 'resume') */
        CallInfo *ci = L->ci;
        ci->u.c.k = k;  /* save continuation */
        ci->u.c.ctx = ctx;  /* save context */
        /* save information for error recovery */
        ci->extra = savestack(L, c.func);
        ci->u.c.old_errfunc = L->errfunc;
        L->errfunc = func;
        setoah(ci->callstatus, L->allowhook);  /* save value of 'allowhook' */
        ci->callstatus |= CIST_YPCALL;  /* function can do error recovery */
        luaD_call(L, c.func, nresults);  /* do the call */
        ci->callstatus &= ~CIST_YPCALL;
        L->errfunc = ci->u.c.old_errfunc;
        status = LUA_OK;  /* if it is here, there were no errors */
    }
	if (status == LUA_OK && (L->state & (lua_VMState::LVM_STATE_BREAK | lua_VMState::LVM_STATE_SUSPEND))) {
		lua_unlock(L);
		return status;
	}
    adjustresults(L, nresults);
    lua_unlock(L);
    return status;
}


LUA_API int lua_load(lua_State *L, lua_Reader reader, void *data,
    const char *chunkname, const char *mode) {
    ZIO z;
    int status;
    lua_lock(L);
    if (!chunkname) chunkname = "?";
    luaZ_init(L, &z, reader, data);
    status = luaD_protectedparser(L, &z, chunkname, mode);
    if (status == LUA_OK) {  /* no errors? */
		uvm_types::GcLClosure *f = clLvalue(L->top - 1);  /* get newly created function */
        if (f->nupvalues >= 1) {  /* does it have an upvalue? */
            /* get global table from registry */
			uvm_types::GcTable *reg = hvalue(&L->l_registry);
            const TValue *gt = luaH_getint(reg, LUA_RIDX_GLOBALS);
            /* set global table as 1st upvalue of 'f' (may be LUA_ENV) */
            setobj(L, f->upvals[0]->v, gt);
            luaC_upvalbarrier(L, f->upvals[0]);
        }
    }
    lua_unlock(L);
    return status;
}

LUA_API int lua_load_with_check(lua_State *L, lua_Reader reader, void *data,
    const char *chunkname, const char *mode, const int check_type) {
    ZIO z;
    int status;
    lua_lock(L);
    if (!chunkname) chunkname = "?";
    luaZ_init(L, &z, reader, data);
    status = luaD_protectedparser(L, &z, chunkname, mode);
    if (status == LUA_OK) {  /* no errors? */
		uvm_types::GcLClosure *f = clLvalue(L->top - 1);  /* get newly created function */
        if (f->nupvalues >= 1) {  /* does it have an upvalue? */
            /* get global table from registry */
			uvm_types::GcTable *reg = hvalue(&L->l_registry);
            const TValue *gt = luaH_getint(reg, LUA_RIDX_GLOBALS);
            /* set global table as 1st upvalue of 'f' (may be LUA_ENV) */
            setobj(L, f->upvals[0]->v, gt);
            luaC_upvalbarrier(L, f->upvals[0]);
        }
    }
    lua_unlock(L);
    return status;
}

LUA_API int lua_dump(lua_State *L, lua_Writer writer, void *data, int strip) {
    int status;
    TValue *o;
    lua_lock(L);
    api_checknelems(L, 1);
    o = L->top - 1;
    if (isLfunction(o))
        status = luaU_dump(L, getproto(o), writer, data, strip);
    else
        status = 1;
    lua_unlock(L);
    return status;
}


LUA_API int lua_status(lua_State *L) {
    return L->status;
}


/*
** Garbage-collection function
*/

LUA_API int lua_gc(lua_State *L, int what, int data) {
	return 0; // TODO: call vmgc's gc method
}



/*
** miscellaneous functions
*/


LUA_API int lua_error(lua_State *L) {
    lua_lock(L);
    api_checknelems(L, 1);
	auto msg = (lua_gettop(L) > 0 && lua_isstring(L, -1)) ? lua_tostring(L, -1) : nullptr;
    luaG_errormsg(L, msg);
    /* code unreachable; will unlock when control actually leaves the kernel */
    return 0;  /* to avoid warnings */
}


LUA_API int lua_next(lua_State *L, int idx) {
    StkId t;
    int more;
    lua_lock(L);
    t = index2addr(L, idx);
    api_check(L, ttistable(t), "table expected");
    more = luaH_next(L, hvalue(t), L->top - 1);
    if (more) {
        api_incr_top(L);
    }
    else  /* no more elements */
        L->top -= 1;  /* remove key */
    lua_unlock(L);
    return more;
}


LUA_API void lua_concat(lua_State *L, int n) {
    lua_lock(L);
    api_checknelems(L, n);
    if (n >= 2) {
        luaC_checkGC(L);
        luaV_concat(nullptr, L, n); // TODO: ctx
    }
    else if (n == 0) {  /* push empty string */
        setsvalue2s(L, L->top, luaS_newlstr(L, "", 0));
        api_incr_top(L);
    }
    /* else n == 1; nothing to do */
    lua_unlock(L);
}


LUA_API void lua_len(lua_State *L, int idx) {
    StkId t;
    lua_lock(L);
    t = index2addr(L, idx);
    luaV_objlen(L, L->top, t);
    api_incr_top(L);
    lua_unlock(L);
}


LUA_API lua_Alloc lua_getallocf(lua_State *L, void **ud) {
    lua_Alloc f;
    lua_lock(L);
    if (ud) *ud = L->ud;
    f = L->frealloc;
    lua_unlock(L);
    return f;
}


LUA_API void lua_setallocf(lua_State *L, lua_Alloc f, void *ud) {
    lua_lock(L);
    L->ud = ud;
    L->frealloc = f;
    lua_unlock(L);
}


LUA_API void *lua_newuserdata(lua_State *L, size_t size) {
    uvm_types::GcUserdata *u;
    lua_lock(L);
    luaC_checkGC(L);
    u = luaS_newudata(L, size);
    setuvalue(L, L->top, u);
    api_incr_top(L);
    lua_unlock(L);
    return getudatamem(u);
}


static const char *aux_upvalue(StkId fi, int n, TValue **val,
    uvm_types::GcCClosure **owner, UpVal **uv) {
    switch (ttype(fi)) {
    case LUA_TCCL: {  /* C closure */
        auto *f = clCvalue(fi);
        if (!(1 <= n && n <= f->nupvalues)) return nullptr;
        *val = &f->upvalue[n - 1];
        if (owner) *owner = f;
        return "";
    }
    case LUA_TLCL: {  /* Lua closure */
		uvm_types::GcLClosure *f = clLvalue(fi);
		uvm_types::GcString *name;
        uvm_types::GcProto *p = f->p;
        if (!(1 <= n && size_t(n) <= p->upvalues.size())) return nullptr;
        *val = f->upvals[n - 1]->v;
        if (uv) *uv = f->upvals[n - 1];
        name = p->upvalues[n - 1].name;
        return (name == nullptr) ? "(*no name)" : getstr(name);
    }
    default: return nullptr;  /* not a closure */
    }
}


LUA_API const char *lua_getupvalue(lua_State *L, int funcindex, int n) {
    const char *name;
    TValue *val = nullptr;  /* to avoid warnings */
    lua_lock(L);
    name = aux_upvalue(index2addr(L, funcindex), n, &val, nullptr, nullptr);
    if (name) {
        setobj2s(L, L->top, val);
        api_incr_top(L);
    }
    lua_unlock(L);
    return name;
}


LUA_API const char *lua_setupvalue(lua_State *L, int funcindex, int n) {
    const char *name;
    TValue *val = nullptr;  /* to avoid warnings */
    uvm_types::GcCClosure *owner = nullptr;
    UpVal *uv = nullptr;
    StkId fi;
    lua_lock(L);
    fi = index2addr(L, funcindex);
    api_checknelems(L, 1);
    name = aux_upvalue(fi, n, &val, &owner, &uv);
    if (name) {
        L->top--;
        setobj(L, val, L->top);
    }
    lua_unlock(L);
    return name;
}


static UpVal **getupvalref(lua_State *L, int fidx, int n, uvm_types::GcLClosure **pf) {
	uvm_types::GcLClosure *f;
    StkId fi = index2addr(L, fidx);
    api_check(L, ttisLclosure(fi), "Lua function expected");
    f = clLvalue(fi);
    api_check(L, (1 <= n && n <= f->p->upvalues.size()), "invalid upvalue index");
    if (pf) *pf = f;
    return &f->upvals[n - 1];  /* get its upvalue pointer */
}


LUA_API void *lua_upvalueid(lua_State *L, int fidx, int n) {
    StkId fi = index2addr(L, fidx);
    switch (ttype(fi)) {
    case LUA_TLCL: {  /* lua closure */
        return *getupvalref(L, fidx, n, nullptr);
    }
    case LUA_TCCL: {  /* C closure */
        auto *f = clCvalue(fi);
        api_check(L, 1 <= n && n <= f->nupvalues, "invalid upvalue index");
        return &f->upvalue[n - 1];
    }
    default: {
        api_check(L, 0, "closure expected");
        return nullptr;
    }
    }
}


LUA_API void lua_upvaluejoin(lua_State *L, int fidx1, int n1,
    int fidx2, int n2) {
	uvm_types::GcLClosure *f1;
    UpVal **up1 = getupvalref(L, fidx1, n1, &f1);
    UpVal **up2 = getupvalref(L, fidx2, n2, nullptr);
    luaC_upvdeccount(L, *up1);
    *up1 = *up2;
    (*up1)->refcount++;
    if (upisopen(*up1)) (*up1)->u.open.touched = 1;
    luaC_upvalbarrier(L, *up1);
}

size_t luaL_traverse_table_with_nested(lua_State *L, int index, lua_table_traverser_with_nested traverser, void *ud, std::list<const void*> &jsons, size_t recur_depth)
{
    if (index > lua_gettop(L))
        return 0;
    if (!lua_istable(L, index))
        return 0;
    lua_len(L, index);
    auto len = (size_t) lua_tointegerx(L, -1, nullptr);
    lua_pop(L, 1);
    size_t keys_count = 0;
    for (size_t i = 0; i < len; ++i)
    {
        lua_pushinteger(L, i + 1);
        auto new_index = index < 0 ? (index - 1) : index;
        lua_geti(L, new_index, i + 1);
        ++keys_count;
        if (nullptr != traverser)
            traverser(L, ud, len, jsons, recur_depth+1);
        lua_pop(L, 2);
    }
    lua_pushvalue(L, index);
    int it = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, it))
    {
        if (lua_isinteger(L, -2))
        {
            auto key_int = lua_tointeger(L, -2);
            if (((size_t)key_int) <= len && key_int > 0)
            {
                lua_pop(L, 1);
                continue;
            }
		}
        ++keys_count;
        if (nullptr != traverser)
            traverser(L, ud, len, jsons, recur_depth+1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return keys_count;
}




size_t luaL_traverse_table(lua_State *L, int index, lua_table_traverser traverser, void *ud)
{
    if (index > lua_gettop(L))
        return 0;
    if (!lua_istable(L, index))
        return 0;
    lua_pushvalue(L, index);
    int it = lua_gettop(L);
    lua_pushnil(L);
    size_t keys_count = 0;
    while (lua_next(L, it))
    {
        keys_count += 1;
        if (nullptr != traverser)
            traverser(L, ud);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return keys_count;
}

size_t luaL_count_global_variables(lua_State *L)
{
    lua_getglobal(L, "_G");
    size_t keys_count = luaL_traverse_table(L, -1, nullptr, nullptr);
    lua_pop(L, 1);
    return keys_count;
}

void luaL_get_global_variables(lua_State *L, std::list<std::string> *list)
{
    if (nullptr == list)
        return;
    lua_getglobal(L, "_G");
    int it = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, it))
    {
        list->push_back(std::string(lua_tostring(L, -2)));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    list->sort();
}