﻿/*
** $Id: ldebug.c,v 2.117 2015/11/02 18:48:07 roberto Exp $
** Debug Interface
** See Copyright Notice in lua.h
*/

#define ldebug_cpp
#define LUA_CORE

#include "uvm/lprefix.h"


#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include "uvm/lua.h"

#include "uvm/lapi.h"
#include "uvm/lcode.h"
#include "uvm/ldebug.h"
#include "uvm/ldo.h"
#include "uvm/lfunc.h"
#include "uvm/lobject.h"
#include "uvm/lopcodes.h"
#include "uvm/lstate.h"
#include "uvm/lstring.h"
#include "uvm/ltable.h"
#include "uvm/ltm.h"
#include "uvm/lvm.h"
#include <uvm/uvm_api.h>

using uvm::lua::api::global_uvm_chain_api;



#define noLuaClosure(f)		((f) == nullptr || (f)->tt_value() == LUA_TCCL)


/* Active Lua function (given call info) */
#define ci_func(ci)		(clLvalue((ci)->func))


static const char *getfuncname(lua_State *L, CallInfo *ci, const char **name);


static int currentpc(CallInfo *ci) {
    lua_assert(isLua(ci));
    return pcRel(ci->u.l.savedpc, ci_func(ci)->p);
}


static int currentline(CallInfo *ci) {
    return getfuncline(ci_func(ci)->p, size_t(currentpc(ci)));
}


/*
** If function yielded, its 'func' can be in the 'extra' field. The
** next function restores 'func' to its correct value for debugging
** purposes. (It exchanges 'func' and 'extra'; so, when called again,
** after debugging, it also "re-restores" ** 'func' to its altered value.
*/
static void swapextra(lua_State *L) {
    if (L->status == LUA_YIELD) {
        CallInfo *ci = L->ci;  /* get function that yielded */
        StkId temp = ci->func;  /* exchange its 'func' and 'extra' values */
        ci->func = restorestack(L, ci->extra);
        ci->extra = savestack(L, temp);
    }
}

LUA_API void lua_set_compile_error(lua_State *L, const char *msg)
{
    if (nullptr != msg && strlen(msg) > 0 && strlen(L->compile_error) < 1)
    {
        auto size = strlen(msg);
        if (size >= LUA_COMPILE_ERROR_MAX_LENGTH)
            size = LUA_COMPILE_ERROR_MAX_LENGTH - 1;
        strncpy(L->compile_error, msg, LUA_COMPILE_ERROR_MAX_LENGTH);
        L->compile_error[size] = '\0';
		L->compile_error[LUA_COMPILE_ERROR_MAX_LENGTH - 1] = '\0';
    }
}

LUA_API void lua_set_run_error(lua_State *L, const char *msg)
{
	if (nullptr != msg && strlen(msg) > 0)
	{
		auto size = strlen(msg);
		if (size >= LUA_VM_EXCEPTION_STRNG_MAX_LENGTH)
			size = LUA_VM_EXCEPTION_STRNG_MAX_LENGTH - 1;
		strncpy(L->runerror, msg, LUA_VM_EXCEPTION_STRNG_MAX_LENGTH);
		L->runerror[size] = '\0';
		global_uvm_chain_api->throw_exception(L, UVM_API_SIMPLE_ERROR, msg);
	}
}



/*
** this function can be called asynchronous (e.g. during a signal)
*/
LUA_API void lua_sethook(lua_State *L, lua_Hook func, int mask, int count) {
    if (func == nullptr || mask == 0) {  /* turn off hooks? */
        mask = 0;
        func = nullptr;
    }
    if (isLua(L->ci))
        L->oldpc = L->ci->u.l.savedpc;
    L->hook = func;
    L->basehookcount = count;
    resethookcount(L);
    L->hookmask = cast_byte(mask);
}


LUA_API lua_Hook lua_gethook(lua_State *L) {
    return L->hook;
}


LUA_API int lua_gethookmask(lua_State *L) {
    return L->hookmask;
}


LUA_API int lua_gethookcount(lua_State *L) {
    return L->basehookcount;
}


LUA_API int lua_getstack(lua_State *L, int level, lua_Debug *ar) {
    int status;
    CallInfo *ci;
    if (level < 0) return 0;  /* invalid (negative) level */
    lua_lock(L);
    for (ci = L->ci; level > 0 && ci != &L->base_ci; ci = ci->previous)
        level--;
    if (level == 0 && ci != &L->base_ci) {  /* level found? */
        status = 1;
        ar->i_ci = ci;
    }
    else status = 0;  /* no such level */
    lua_unlock(L);
    return status;
}


static const char *upvalname(uvm_types::GcProto *p, int uv) {
	uvm_types::GcString *s = check_exp(uv < p->upvalues.size(), p->upvalues[uv].name);
    if (s == nullptr) return "?";
    else return getstr(s);
}


static const char *findvararg(CallInfo *ci, int n, StkId *pos) {
    int nparams = clLvalue(ci->func)->p->numparams;
    if (n >= cast_int(ci->u.l.base - ci->func) - nparams)
        return nullptr;  /* no such vararg */
    else {
        *pos = ci->func + nparams + n;
        return "(*vararg)";  /* generic name for any vararg */
    }
}


static const char *findlocal(lua_State *L, CallInfo *ci, int n,
    StkId *pos) {
    const char *name = nullptr;
    StkId base;
    if (isLua(ci)) {
        if (n < 0)  /* access to vararg values? */
            return findvararg(ci, -n, pos);
        else {
            base = ci->u.l.base;
            name = luaF_getlocalname(ci_func(ci)->p, n, currentpc(ci));
        }
    }
    else
        base = ci->func + 1;
    if (name == nullptr) {  /* no 'standard' name? */
        StkId limit = (ci == L->ci) ? L->top : ci->next->func;
        if (limit - base >= n && n > 0)  /* is 'n' inside 'ci' stack? */
            name = "(*temporary)";  /* generic name for any valid slot */
        else
            return nullptr;  /* no name */
    }
    *pos = base + (n - 1);
    return name;
}


LUA_API const char *lua_getlocal(lua_State *L, const lua_Debug *ar, int n) {
    const char *name;
    lua_lock(L);
    swapextra(L);
    if (ar == nullptr) {  /* information about non-active function? */
        if (!isLfunction(L->top - 1))  /* not a Lua function? */
            name = nullptr;
        else  /* consider live variables at function start (parameters) */
            name = luaF_getlocalname(clLvalue(L->top - 1)->p, n, 0);
    }
    else {  /* active function; get information through 'ar' */
        StkId pos = nullptr;  /* to avoid warnings */
        name = findlocal(L, ar->i_ci, n, &pos);
        if (name) {
            setobj2s(L, L->top, pos);
            api_incr_top(L);
        }
    }
    swapextra(L);
    lua_unlock(L);
    return name;
}


LUA_API const char *lua_setlocal(lua_State *L, const lua_Debug *ar, int n) {
    StkId pos = nullptr;  /* to avoid warnings */
    const char *name;
    lua_lock(L);
    swapextra(L);
    name = findlocal(L, ar->i_ci, n, &pos);
    if (name) {
        setobjs2s(L, pos, L->top - 1);
        L->top--;  /* pop value */
    }
    swapextra(L);
    lua_unlock(L);
    return name;
}


static void funcinfo(lua_Debug *ar, uvm_types::GcClosure *cl) {
    if (noLuaClosure(cl)) {
        ar->source = "=[C]";
        ar->linedefined = -1;
        ar->lastlinedefined = -1;
        ar->what = "C";
    }
    else {
		uvm_types::GcProto *p = static_cast<uvm_types::GcLClosure*>(cl)->p;
        ar->source = p->source ? getstr(p->source) : "=?";
        ar->linedefined = p->linedefined;
        ar->lastlinedefined = p->lastlinedefined;
        ar->what = (ar->linedefined == 0) ? "main" : "Lua";
    }
    luaO_chunkid(ar->short_src, ar->source, LUA_IDSIZE);
}


static void collectvalidlines(lua_State *L, uvm_types::GcClosure *f) {
    if (noLuaClosure(f)) {
        setnilvalue(L->top);
        api_incr_top(L);
    }
    else {
        size_t i;
        TValue v;
        int *lineinfo = static_cast<uvm_types::GcLClosure*>(f)->p->lineinfos.empty() ? nullptr : static_cast<uvm_types::GcLClosure*>(f)->p->lineinfos.data();
        uvm_types::GcTable *t = luaH_new(L);  /* new table to store active lines */
        sethvalue(L, L->top, t);  /* push it on stack */
        api_incr_top(L);
        setbvalue(&v, 1);  /* boolean 'true' to be the value of all indices */
        for (i = 0; i < static_cast<uvm_types::GcLClosure*>(f)->p->lineinfos.size(); i++)  /* for all lines with code */
            luaH_setint(L, t, lineinfo[i], &v);  /* table[line] = true */
    }
}


static int auxgetinfo(lua_State *L, const char *what, lua_Debug *ar,
	uvm_types::GcClosure *f, CallInfo *ci) {
    int status = 1;
    for (; *what; what++) {
        switch (*what) {
        case 'S': {
            funcinfo(ar, f);
            break;
        }
        case 'l': {
            ar->currentline = (ci && isLua(ci)) ? currentline(ci) : -1;
            break;
        }
        case 'u': {
			ar->nups = (f == nullptr) ? 0 : f->nupvalues_count();
            if (noLuaClosure(f)) {
                ar->isvararg = 1;
                ar->nparams = 0;
            }
            else {
                ar->isvararg = static_cast<uvm_types::GcLClosure*>(f)->p->is_vararg;
                ar->nparams = static_cast<uvm_types::GcLClosure*>(f)->p->numparams;
            }
            break;
        }
        case 't': {
            ar->istailcall = (ci) ? ci->callstatus & CIST_TAIL : 0;
            break;
        }
        case 'n': {
            /* calling function is a known Lua function? */
            if (ci && !(ci->callstatus & CIST_TAIL) && isLua(ci->previous))
                ar->namewhat = getfuncname(L, ci->previous, &ar->name);
            else
                ar->namewhat = nullptr;
            if (ar->namewhat == nullptr) {
                ar->namewhat = "";  /* not found */
                ar->name = nullptr;
            }
            break;
        }
        case 'L':
        case 'f':  /* handled by lua_getinfo */
            break;
        default: status = 0;  /* invalid option */
        }
    }
    return status;
}


LUA_API int lua_getinfo(lua_State *L, const char *what, lua_Debug *ar) {
    int status;
    uvm_types::GcClosure *cl;
    CallInfo *ci;
    StkId func;
    lua_lock(L);
    swapextra(L);
    if (*what == '>') {
        ci = nullptr;
        func = L->top - 1;
        api_check(L, ttisfunction(func), "function expected");
        what++;  /* skip the '>' */
        L->top--;  /* pop function */
    }
    else {
        ci = ar->i_ci;
        func = ci->func;
        lua_assert(ttisfunction(ci->func));
    }
    cl = ttisclosure(func) ? clvalue(func) : nullptr;
    status = auxgetinfo(L, what, ar, cl, ci);
    if (strchr(what, 'f')) {
        setobjs2s(L, L->top, func);
        api_incr_top(L);
    }
    swapextra(L);  /* correct before option 'L', which can raise a mem. error */
    if (strchr(what, 'L'))
        collectvalidlines(L, cl);
    lua_unlock(L);
    return status;
}


/*
** {======================================================
** Symbolic Execution
** =======================================================
*/

static const char *getobjname(uvm_types::GcProto *p, int lastpc, int reg,
    const char **name);


/*
** find a "name" for the RK value 'c'
*/
static void kname(uvm_types::GcProto *p, int pc, int c, const char **name) {
    if (ISK(c)) {  /* is 'c' a constant? */
        TValue *kvalue = &p->ks[INDEXK(c)];
        if (ttisstring(kvalue)) {  /* literal constant? */
            *name = svalue(kvalue);  /* it is its own name */
            return;
        }
        /* else no reasonable name found */
    }
    else {  /* 'c' is a register */
        const char *what = getobjname(p, pc, c, name); /* search for 'c' */
        if (what && *what == 'c') {  /* found a constant name? */
            return;  /* 'name' already filled */
        }
        /* else no reasonable name found */
    }
    *name = "?";  /* no reasonable name found */
}


static int filterpc(int pc, int jmptarget) {
    if (pc < jmptarget)  /* is code conditional (inside a jump)? */
        return -1;  /* cannot know who sets that register */
    else return pc;  /* current position sets that register */
}


/*
** try to find last instruction before 'lastpc' that modified register 'reg'
*/
static int findsetreg(uvm_types::GcProto *p, int lastpc, int reg) {
    int pc;
    int setreg = -1;  /* keep last instruction that changed 'reg' */
    int jmptarget = 0;  /* any code before this address is conditional */
    for (pc = 0; pc < lastpc; pc++) {
        Instruction i = p->codes[pc];
        OpCode op = GET_OPCODE(i);
        int a = GETARG_A(i);
        switch (op) {
        case UOP_LOADNIL: {
            int b = GETARG_B(i);
            if (a <= reg && reg <= a + b)  /* set registers from 'a' to 'a+b' */
                setreg = filterpc(pc, jmptarget);
            break;
        }
        case UOP_TFORCALL: {
            if (reg >= a + 2)  /* affect all regs above its base */
                setreg = filterpc(pc, jmptarget);
            break;
        }
        case UOP_CALL:
        case UOP_TAILCALL: {
            if (reg >= a)  /* affect all registers above base */
                setreg = filterpc(pc, jmptarget);
            break;
        }
        case UOP_JMP: {
            int b = GETARG_sBx(i);
            int dest = pc + 1 + b;
            /* jump is forward and do not skip 'lastpc'? */
            if (pc < dest && dest <= lastpc) {
                if (dest > jmptarget)
                    jmptarget = dest;  /* update 'jmptarget' */
            }
            break;
        }
        default:
            if (testAMode(op) && reg == a)  /* any instruction that set A */
                setreg = filterpc(pc, jmptarget);
            break;
        }
    }
    return setreg;
}


static const char *getobjname(uvm_types::GcProto *p, int lastpc, int reg,
    const char **name) {
    int pc;
    *name = luaF_getlocalname(p, reg + 1, lastpc);
    if (*name)  /* is a local? */
        return "local";
    /* else try symbolic execution */
    pc = findsetreg(p, lastpc, reg);
    if (pc != -1) {  /* could find instruction? */
        Instruction i = p->codes[pc];
        OpCode op = GET_OPCODE(i);
        switch (op) {
        case UOP_MOVE: {
            int b = GETARG_B(i);  /* move from 'b' to 'a' */
            if (b < GETARG_A(i))
                return getobjname(p, pc, b, name);  /* get name for 'b' */
            break;
        }
        case UOP_GETTABUP:
        case UOP_GETTABLE: {
            int k = GETARG_C(i);  /* key index */
            int t = GETARG_B(i);  /* table index */
            const char *vn = (op == UOP_GETTABLE)  /* name of indexed variable */
                ? luaF_getlocalname(p, t + 1, pc)
                : upvalname(p, t);
            kname(p, pc, k, name);
            return (vn && strcmp(vn, LUA_ENV) == 0) ? "global" : "field";
        }
        case UOP_GETUPVAL: {
            *name = upvalname(p, GETARG_B(i));
            return "upvalue";
        }
        case UOP_LOADK:
        case UOP_LOADKX: {
            int b = (op == UOP_LOADK) ? GETARG_Bx(i)
                : GETARG_Ax(p->codes[pc + 1]);
            if (ttisstring(&p->ks[b])) {
                *name = svalue(&p->ks[b]);
                return "constant";
            }
            break;
        }
        case UOP_SELF: {
            int k = GETARG_C(i);  /* key index */
            kname(p, pc, k, name);
            return "method";
        }
        default: break;  /* go through to return nullptr */
        }
    }
    return nullptr;  /* could not find reasonable name */
}


static const char *getfuncname(lua_State *L, CallInfo *ci, const char **name) {
    TMS tm = (TMS)0;  /* to avoid warnings */
	uvm_types::GcProto *p = ci_func(ci)->p;  /* calling function */
    int pc = currentpc(ci);  /* calling instruction index */
    Instruction i = p->codes[pc];  /* calling instruction */
    if (ci->callstatus & CIST_HOOKED) {  /* was it called inside a hook? */
        *name = "?";
        return "hook";
    }
    switch (GET_OPCODE(i)) {
    case UOP_CALL:
    case UOP_TAILCALL:  /* get function name */
        return getobjname(p, pc, GETARG_A(i), name);
    case UOP_TFORCALL: {  /* for iterator */
        *name = "for iterator";
        return "for iterator";
    }
                      /* all other instructions can call only through metamethods */
    case UOP_SELF: case UOP_GETTABUP: case UOP_GETTABLE:
        tm = TM_INDEX;
        break;
    case UOP_SETTABUP: case UOP_SETTABLE:
        tm = TM_NEWINDEX;
        break;
    case UOP_ADD: case UOP_SUB: case UOP_MUL: case UOP_MOD:
    case UOP_POW: case UOP_DIV: case UOP_IDIV: case UOP_BAND:
    case UOP_BOR: case UOP_BXOR: case UOP_SHL: case UOP_SHR: {
        int offset = cast_int(GET_OPCODE(i)) - cast_int(UOP_ADD);  /* ORDER OP */
        tm = lua_cast(TMS, offset + cast_int(TM_ADD));  /* ORDER TM */
        break;
    }
    case UOP_UNM: tm = TM_UNM; break;
    case UOP_BNOT: tm = TM_BNOT; break;
    case UOP_LEN: tm = TM_LEN; break;
    case UOP_CONCAT: tm = TM_CONCAT; break;
    case UOP_EQ: tm = TM_EQ; break;
    case UOP_LT: tm = TM_LT; break;
    case UOP_LE: tm = TM_LE; break;
    default: lua_assert(0);  /* other instructions cannot call a function */
    }
    *name = getstr(L->tmname[tm]);
    return "metamethod";
}

/* }====================================================== */



/*
** The subtraction of two potentially unrelated pointers is
** not ISO C, but it should not crash a program; the subsequent
** checks are ISO C and ensure a correct result.
*/
static int isinstack(CallInfo *ci, const TValue *o) {
    ptrdiff_t i = o - ci->u.l.base;
    return (0 <= i && i < (ci->top - ci->u.l.base) && ci->u.l.base + i == o);
}


/*
** Checks whether value 'o' came from an upvalue. (That can only happen
** with instructions UOP_GETTABUP/OP_SETTABUP, which operate directly on
** upvalues.)
*/
static const char *getupvalname(CallInfo *ci, const TValue *o,
    const char **name) {
	uvm_types::GcLClosure *c = ci_func(ci);
    int i;
    for (i = 0; i < c->nupvalues; i++) {
		if (!c->upvals[i])
		{
			*name = "";
			return "upvalue";
		}
        if (c->upvals[i]->v == o) {
            *name = upvalname(c->p, i);
            return "upvalue";
        }
    }
    return nullptr;
}


static const char *varinfo(lua_State *L, const TValue *o) {
    const char *name = nullptr;  /* to avoid warnings */
    CallInfo *ci = L->ci;
    const char *kind = nullptr;
    if (isLua(ci)) {
		// FIXME: when L is error, name can't get data, eg. import not existed contract
        kind = getupvalname(ci, o, &name);  /* check whether 'o' is an upvalue */
        if (!kind && isinstack(ci, o))  /* no? try a register */
            kind = getobjname(ci_func(ci)->p, currentpc(ci),
            cast_int(o - ci->u.l.base), &name);
    }
    return (kind) ? luaO_pushfstring(L, " (%s '%s')", kind, name) : "";
}


void luaG_typeerror(lua_State *L, const TValue *o, const char *op) {
    const char *t = objtypename(o);
    luaG_runerror(L, "attempt to %s a %s value%s", op, t, varinfo(L, o));
}


void luaG_concaterror(lua_State *L, const TValue *p1, const TValue *p2) {
    if (ttisstring(p1) || cvt2str(p1)) p1 = p2;
    luaG_typeerror(L, p1, "concatenate");
}


void luaG_opinterror(lua_State *L, const TValue *p1,
    const TValue *p2, const char *msg) {
    lua_Number temp;
    if (!tonumber(p1, &temp))  /* first operand is wrong? */
        p2 = p1;  /* now second is wrong */
    luaG_typeerror(L, p2, msg);
}


/*
** Error when both values are convertible to numbers, but not to integers
*/
void luaG_tointerror(lua_State *L, const TValue *p1, const TValue *p2) {
    lua_Integer temp;
    if (!tointeger(p1, &temp))
        p2 = p1;
    luaG_runerror(L, "number%s has no integer representation", varinfo(L, p2));
}


void luaG_ordererror(lua_State *L, const TValue *p1, const TValue *p2) {
    const char *t1 = objtypename(p1);
    const char *t2 = objtypename(p2);
    if (t1 == t2)
        luaG_runerror(L, "attempt to compare two %s values", t1);
    else
        luaG_runerror(L, "attempt to compare %s with %s", t1, t2);
}


/* add src:line information to 'msg' */
const char *luaG_addinfo(lua_State *L, const char *msg, uvm_types::GcString *src,
    int line) {
    char buff[LUA_IDSIZE];
    if (src)
        luaO_chunkid(buff, getstr(src), LUA_IDSIZE);
    else {  /* no source available; use "?" instead */
        buff[0] = '?'; buff[1] = '\0';
    }
    return luaO_pushfstring(L, "%s:%d: %s", buff, line, msg);
}


void luaG_errormsg(lua_State *L, const char *msg) {
    if (L->errfunc != 0) {  /* is there an error handling function? */
        StkId errfunc = restorestack(L, L->errfunc);
        setobjs2s(L, L->top, L->top - 1);  /* move argument */
        setobjs2s(L, L->top - 1, errfunc);  /* push function */
        L->top++;  /* assume EXTRA_STACK */
        luaD_callnoyield(L, L->top - 2, 1);  /* call it */
    }
	if(nullptr != msg) 
		lua_set_run_error(L, msg);
    luaD_throw(L, LUA_ERRRUN);
}


void luaG_runerror(lua_State *L, const char *fmt, ...) {
    CallInfo *ci = L->ci;
    const char *msg;
    va_list argp;
    va_start(argp, fmt);
    msg = luaO_pushvfstring(L, fmt, argp);  /* format message */
    va_end(argp);
    if (isLua(ci))  /* if Lua function, add source:line information */
        luaG_addinfo(L, msg, ci_func(ci)->p->source, currentline(ci));
	global_uvm_chain_api->throw_exception(L, UVM_API_LVM_ERROR, msg);
    luaG_errormsg(L, msg);
}


void luaG_traceexec(lua_State *L) {
    CallInfo *ci = L->ci;
    lu_byte mask = L->hookmask;
    int counthook = (--L->hookcount == 0 && (mask & LUA_MASKCOUNT));
    if (counthook)
        resethookcount(L);  /* reset count */
    else if (!(mask & LUA_MASKLINE))
        return;  /* no line hook and count != 0; nothing to be done */
    if (ci->callstatus & CIST_HOOKYIELD) {  /* called hook last time? */
        ci->callstatus &= ~CIST_HOOKYIELD;  /* erase mark */
        return;  /* do not call hook again (VM yielded, so it did not move) */
    }
    if (counthook)
        luaD_hook(L, LUA_HOOKCOUNT, -1);  /* call count hook */
    if (mask & LUA_MASKLINE) {
		uvm_types::GcProto *p = ci_func(ci)->p;
        int npc = pcRel(ci->u.l.savedpc, p);
        auto newline = getfuncline(p, size_t(npc));
		if (npc == 0 ||  /* call linehook when enter a new function, */
			ci->u.l.savedpc <= L->oldpc ||  /* when jump back (loop), or when */
			newline != getfuncline(p, size_t(pcRel(L->oldpc, p))))  /* enter a new line */
            luaD_hook(L, LUA_HOOKLINE, newline);  /* call line hook */
    }
    L->oldpc = ci->u.l.savedpc;
    if (L->status == LUA_YIELD) {  /* did hook yield? */
        if (counthook)
            L->hookcount = 1;  /* undo decrement to zero */
        ci->u.l.savedpc--;  /* undo increment (resume will increment it again) */
        ci->callstatus |= CIST_HOOKYIELD;  /* mark that it yielded */
        ci->func = L->top - 1;  /* protect stack below results */
        luaD_throw(L, LUA_YIELD);
    }
}

