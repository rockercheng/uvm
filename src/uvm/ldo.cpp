﻿/*
** $Id: ldo.c,v 2.150 2015/11/19 19:16:22 roberto Exp $
** Stack and Call structure of Lua
** See Copyright Notice in lua.h
*/

#define ldo_cpp
#define LUA_CORE

#include "uvm/lprefix.h"


#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "uvm/lua.h"

#include "uvm/lapi.h"
#include "uvm/ldebug.h"
#include "uvm/ldo.h"
#include "uvm/lfunc.h"
#include "uvm/lgc.h"
#include "uvm/lmem.h"
#include "uvm/lobject.h"
#include "uvm/lopcodes.h"
#include "uvm/lparser.h"
#include "uvm/lstate.h"
#include "uvm/lstring.h"
#include "uvm/ltable.h"
#include "uvm/ltm.h"
#include "uvm/lundump.h"
#include "uvm/lvm.h"
#include "uvm/lzio.h"
#include "uvm/uvm_lib.h"

using uvm::lua::api::global_uvm_chain_api;


#define errorstatus(s)	((s) > LUA_YIELD)


/*
** {======================================================
** Error-recovery functions
** =======================================================
*/

/*
** LUAI_THROW/LUAI_TRY define how Lua does exception handling. By
** default, Lua handles errors with exceptions when compiling as
** C++ code, with _longjmp/_setjmp when asked to use them, and with
** longjmp/setjmp otherwise.
*/
#if !defined(LUAI_THROW)				/* { */

#if defined(__cplusplus) && !defined(LUA_USE_LONGJMP)	/* { */

/* C++ exceptions */
#define LUAI_THROW(L,c)		throw(c)
#define LUAI_TRY(L,c,a) \
	try { a } catch(...) { if ((c)->status == 0) (c)->status = -1; }
#define luai_jmpbuf		int  /* dummy variable */

#elif defined(LUA_USE_POSIX)				/* }{ */

/* in POSIX, try _longjmp/_setjmp (more efficient) */
#define LUAI_THROW(L,c)		_longjmp((c)->b, 1)
#define LUAI_TRY(L,c,a)		if (_setjmp((c)->b) == 0) { a }
#define luai_jmpbuf		jmp_buf

#else							/* }{ */

/* ISO C handling with long jumps */
#define LUAI_THROW(L,c)		longjmp((c)->b, 1)
#define LUAI_TRY(L,c,a)		if (setjmp((c)->b) == 0) { a }
#define luai_jmpbuf		jmp_buf

#endif							/* } */

#endif							/* } */



/* chain list of long jump buffers */
struct lua_longjmp {
    struct lua_longjmp *previous;
    luai_jmpbuf b;
    volatile int status;  /* error code */
};

static std::string geterrorobjstr(lua_State *L, int errcode)
{
    switch (errcode) {
    case LUA_ERRMEM: {  /* memory error? */
        return "memory error";
    }
    case LUA_ERRERR: {
        return "error in error handling";
    }
    default: {
        return lua_tolstring(L, -1, nullptr);
        // setobjs2s(L, oldtop, L->top - 1);  /* error message on current top */
    }
    }

}


static void seterrorobj(lua_State *L, int errcode, StkId oldtop) {
    switch (errcode) {
    case LUA_ERRMEM: {  /* memory error? */
        setsvalue2s(L, oldtop, L->memerrmsg); /* reuse preregistered msg. */
        break;
    }
    case LUA_ERRERR: {
        setsvalue2s(L, oldtop, luaS_newliteral(L, "error in error handling"));
        break;
    }
    default: {
        setobjs2s(L, oldtop, L->top - 1);  /* error message on current top */
        break;
    }
    }
    L->top = oldtop + 1;
}


void luaD_throw(lua_State *L, int errcode) {
    if (L->errorJmp) {  // thread has an error handler?
        L->errorJmp->status = errcode;  // set status
        LUAI_THROW(L, L->errorJmp);  // jump to it
    }
    else {  // thread has no error handler 
		std::string errmsg;
		if (L->panic) {  // panic function?
			seterrorobj(L, errcode, L->top);  // assume EXTRA_STACK
			if (L->ci->top < L->top)
				L->ci->top = L->top;  // pushing msg. can break this invariant
			lua_unlock(L);
			errmsg = geterrorobjstr(L, errcode);
			L->panic(L);  // call panic function (last chance to jump out)
		}
		else
		{
			errmsg = "not found global function";
		}
		// abort();
		global_uvm_chain_api->throw_exception(L, UVM_API_SIMPLE_ERROR, errmsg.c_str());
		uvm::lua::lib::notify_lua_state_stop(L);
		L->force_stopping = true;
    }
}


int luaD_rawrunprotected(lua_State *L, Pfunc f, void *ud) {
    unsigned short oldnCcalls = L->nCcalls;
    struct lua_longjmp lj;
    lj.status = LUA_OK;
    lj.previous = L->errorJmp;  /* chain new error handler */
    L->errorJmp = &lj;
    LUAI_TRY(L, &lj,
        (*f)(L, ud);
    );
	if (L->state & (lua_VMState::LVM_STATE_BREAK | lua_VMState::LVM_STATE_SUSPEND)) {
		return lj.status;
	}
    L->errorJmp = lj.previous;  /* restore old error handler */
    L->nCcalls = oldnCcalls;
    return lj.status;
}

/* }====================================================== */


/*
** {==================================================================
** Stack reallocation
** ===================================================================
*/
static void correctstack(lua_State *L, TValue *oldstack) {
    CallInfo *ci;
    UpVal *up;
    L->top = (L->top - oldstack) + L->stack;
    for (up = L->openupval; up != nullptr; up = up->u.open.next)
        up->v = (up->v - oldstack) + L->stack;
    for (ci = L->ci; ci != nullptr; ci = ci->previous) {
        ci->top = (ci->top - oldstack) + L->stack;
        ci->func = (ci->func - oldstack) + L->stack;
        if (isLua(ci))
            ci->u.l.base = (ci->u.l.base - oldstack) + L->stack;
    }
}


/* some space for error handling */
#define ERRORSTACKSIZE	(LUAI_MAXSTACK + 200)


void luaD_reallocstack(lua_State *L, int newsize) {
    TValue *oldstack = L->stack;
    int lim = L->stacksize;
    lua_assert(newsize <= LUAI_MAXSTACK || newsize == ERRORSTACKSIZE);
    lua_assert(L->stack_last - L->stack == L->stacksize - EXTRA_STACK);

	auto newstack = static_cast<TValue*>(L->gc_state->gc_malloc_vector(newsize, sizeof(TValue)));
	memcpy(newstack, L->stack, sizeof(TValue) * L->stacksize);
	L->stack = newstack;
    for (; lim < newsize; lim++)
        setnilvalue(L->stack + lim); /* erase new segment */
    L->stacksize = newsize;
    L->stack_last = L->stack + newsize - EXTRA_STACK;
    correctstack(L, oldstack);
}


void luaD_growstack(lua_State *L, int n) {
    int size = L->stacksize;
    if (size > LUAI_MAXSTACK)  /* error after extra size? */
        luaD_throw(L, LUA_ERRERR);
    else {
        int needed = cast_int(L->top - L->stack) + n + EXTRA_STACK;
        int newsize = 2 * size;
        if (newsize > LUAI_MAXSTACK) newsize = LUAI_MAXSTACK;
        if (newsize < needed) newsize = needed;
        if (newsize > LUAI_MAXSTACK) {  /* stack overflow? */
            luaD_reallocstack(L, ERRORSTACKSIZE);
            luaG_runerror(L, "stack overflow");
        }
        else
            luaD_reallocstack(L, newsize);
    }
}


static int stackinuse(lua_State *L) {
    CallInfo *ci;
    StkId lim = L->top;
    for (ci = L->ci; ci != nullptr; ci = ci->previous) {
        lua_assert(ci->top <= L->stack_last);
        if (lim < ci->top) lim = ci->top;
    }
    return cast_int(lim - L->stack) + 1;  /* part of stack in use */
}


void luaD_shrinkstack(lua_State *L) {
    int inuse = stackinuse(L);
    int goodsize = inuse + (inuse / 8) + 2 * EXTRA_STACK;
    if (goodsize > LUAI_MAXSTACK) goodsize = LUAI_MAXSTACK;
    if (L->stacksize > LUAI_MAXSTACK)  /* was handling stack overflow? */
        luaE_freeCI(L);  /* free all CIs (list grew because of an error) */
    else
        luaE_shrinkCI(L);  /* shrink list */
    if (inuse <= LUAI_MAXSTACK &&  /* not handling stack overflow? */
        goodsize < L->stacksize)  /* trying to shrink? */
        luaD_reallocstack(L, goodsize);  /* shrink it */
    else
        condmovestack(L, , );  /* don't change stack (change only for debugging) */
}


void luaD_inctop(lua_State *L) {
    luaD_checkstack(L, 1);
    L->top++;
}

/* }================================================================== */


void luaD_hook(lua_State *L, int event, int line) {
    lua_Hook hook = L->hook;
    if (hook && L->allowhook) {
        CallInfo *ci = L->ci;
        ptrdiff_t top = savestack(L, L->top);
        ptrdiff_t ci_top = savestack(L, ci->top);
        lua_Debug ar;
        ar.event = event;
        ar.currentline = line;
        ar.i_ci = ci;
        luaD_checkstack(L, LUA_MINSTACK);  /* ensure minimum stack size */
        ci->top = L->top + LUA_MINSTACK;
        lua_assert(ci->top <= L->stack_last);
        L->allowhook = 0;  /* cannot call hooks inside a hook */
        ci->callstatus |= CIST_HOOKED;
        lua_unlock(L);
        (*hook)(L, &ar);
        lua_lock(L);
        lua_assert(!L->allowhook);
        L->allowhook = 1;
        ci->top = restorestack(L, ci_top);
        L->top = restorestack(L, top);
        ci->callstatus &= ~CIST_HOOKED;
    }
}


static void callhook(lua_State *L, CallInfo *ci) {
    int hook = LUA_HOOKCALL;
    ci->u.l.savedpc++;  /* hooks assume 'pc' is already incremented */
    if (isLua(ci->previous) &&
        GET_OPCODE(*(ci->previous->u.l.savedpc - 1)) == UOP_TAILCALL) {
        ci->callstatus |= CIST_TAIL;
        hook = LUA_HOOKTAILCALL;
    }
    luaD_hook(L, hook, -1);
    ci->u.l.savedpc--;  /* correct 'pc' */
}


static StkId adjust_varargs(lua_State *L, uvm_types::GcProto *p, int actual) {
    int i;
    int nfixargs = p->numparams;
    StkId base, fixed;
    /* move fixed parameters to final position */
    fixed = L->top - actual;  /* first fixed argument */
    base = L->top;  /* final position of first argument */
    for (i = 0; i < nfixargs && i < actual; i++) {
        setobjs2s(L, L->top++, fixed + i);
        setnilvalue(fixed + i);  /* erase original copy (for GC) */
    }
    for (; i < nfixargs; i++)
        setnilvalue(L->top++);  /* complete missing arguments */
    return base;
}


/*
** Check whether __call metafield of 'func' is a function. If so, put
** it in stack below original 'func' so that 'luaD_precall' can call
** it. Raise an error if __call metafield is not a function.
*/
static void tryfuncTM(lua_State *L, StkId func) {
    const TValue *tm = luaT_gettmbyobj(L, func, TM_CALL);
    StkId p;
    if (!ttisfunction(tm))
    {
        global_uvm_chain_api->throw_exception(L, UVM_API_LVM_ERROR, "Can't find __call method");
        luaG_typeerror(L, func, "call");
    }
    if (L->force_stopping)
        return;
    /* Open a hole inside the stack at 'func' */
    for (p = L->top; p > func; p--)
        setobjs2s(L, p, p - 1);
    L->top++;  /* slot ensured by caller */
    setobj2s(L, func, tm);  /* tag method is the new function to be called */
}



#define next_ci(L) (L->ci = (L->ci->next ? L->ci->next : luaE_extendCI(L)))


/* macro to check stack size, preserving 'p' */
#define checkstackp(L,n,p)  \
  luaD_checkstackaux(L, n, \
    ptrdiff_t t__ = savestack(L, p);  /* save 'p' */ \
    luaC_checkGC(L),  /* stack grow uses memory */ \
    p = restorestack(L, t__))  /* 'pos' part: restore 'p' */


/*
** Prepares a function call: checks the stack, creates a new CallInfo
** entry, fills in the relevant information, calls hook if needed.
** If function is a C function, does the call, too. (Otherwise, leave
** the execution ('luaV_execute') to the caller, to allow stackless
** calls.) Returns true iff function has been executed (C function).
*/
int luaD_precall(lua_State *L, StkId func, int nresults) {
    lua_CFunction f;
    CallInfo *ci;
	L->ci_depth++;
    switch (ttype(func)) {
    case LUA_TCCL:  /* C closure */
        f = clCvalue(func)->f;
        goto Cfunc;
    case LUA_TLCF:  /* light C function */
        f = fvalue(func);
    Cfunc: {
        int n;  /* number of returns */
        checkstackp(L, LUA_MINSTACK, func);  /* ensure minimum stack size */
        ci = next_ci(L);  /* now 'enter' new function */
        ci->nresults = nresults;
        ci->func = func;
        ci->top = L->top + LUA_MINSTACK;
        lua_assert(ci->top <= L->stack_last);
        ci->callstatus = 0;
        if (L->hookmask & LUA_MASKCALL)
            luaD_hook(L, LUA_HOOKCALL, -1);
        lua_unlock(L);
        n = (*f)(L);  /* do the actual call */
        lua_lock(L);
		if (L->state & (lua_VMState::LVM_STATE_BREAK | lua_VMState::LVM_STATE_SUSPEND)) {
			return 1;
		}
		api_checknelems(L, n);
		luaD_poscall(L, ci, L->top - n, n);
		// set last_return
		bool use_last_return = true;
		if (use_last_return) {
			if (n > 0) {
				lua_getglobal(L, "_G");
				lua_pushvalue(L, -n - 1);
				lua_setfield(L, -2, "last_return");
				lua_pop(L, 1);
			}
			else {
				lua_getglobal(L, "_G");
				lua_pushnil(L);
				lua_setfield(L, -2, "last_return");
				lua_pop(L, 1);
			}
		}
        return 1;
    }
    case LUA_TLCL: {  /* Lua function: prepare its call */
        StkId base;
		uvm_types::GcProto *p = clLvalue(func)->p;
        int n = cast_int(L->top - func) - 1;  /* number of real arguments */
        int fsize = p->maxstacksize;  /* frame size */
        checkstackp(L, fsize, func);
        if (p->is_vararg != 1) {  /* do not use vararg? */
            for (; n < p->numparams; n++)
                setnilvalue(L->top++);  /* complete missing arguments */
            base = func + 1;
        }
        else
            base = adjust_varargs(L, p, n);
        ci = next_ci(L);  /* now 'enter' new function */
        ci->nresults = nresults;
        ci->func = func;
        ci->u.l.base = base;
        L->top = ci->top = base + fsize;
        lua_assert(ci->top <= L->stack_last);
        ci->u.l.savedpc = p->codes.empty() ? nullptr : p->codes.data();  /* starting point */
        ci->callstatus = CIST_LUA;
        if (L->hookmask & LUA_MASKCALL)
            callhook(L, ci);
        return 0;
    }
    default: {  /* not a function */
        checkstackp(L, 1, func);  /* ensure space for metamethod */
        tryfuncTM(L, func);  /* try to get '__call' metamethod */
        if (L->force_stopping)
            return false;
        return luaD_precall(L, func, nresults);  /* now it must be a function */
    }
    }
}


/*
** Given 'nres' results at 'firstResult', move 'wanted' of them to 'res'.
** Handle most typical cases (zero results for commands, one result for
** expressions, multiple results for tail calls/single parameters)
** separated.
*/
static int moveresults(lua_State *L, const TValue *firstResult, StkId res,
    int nres, int wanted) {
    switch (wanted) {  /* handle typical cases separately */
    case 0: break;  /* nothing to move */
    case 1: {  /* one result needed */
        if (nres == 0)   /* no results? */
            firstResult = luaO_nilobject;  /* adjust with nil */
        setobjs2s(L, res, firstResult);  /* move it to proper place */
        break;
    }
    case LUA_MULTRET: {
        int i;
        for (i = 0; i < nres; i++)  /* move all results to correct place */
            setobjs2s(L, res + i, firstResult + i);
        L->top = res + nres;
        return 0;  /* wanted == LUA_MULTRET */
    }
    default: {
        int i;
        if (wanted <= nres) {  /* enough results? */
            for (i = 0; i < wanted; i++)  /* move wanted results to correct place */
                setobjs2s(L, res + i, firstResult + i);
        }
        else {  /* not enough results; use all of them plus nils */
            for (i = 0; i < nres; i++)  /* move all results to correct place */
                setobjs2s(L, res + i, firstResult + i);
            for (; i < wanted; i++)  /* complete wanted number of results */
                setnilvalue(res + i);
        }
        break;
    }
    }
    L->top = res + wanted;  /* top points after the last result */
    return 1;
}


/*
** Finishes a function call: calls hook if necessary, removes CallInfo,
** moves current number of results to proper place; returns 0 iff call
** wanted multiple (variable number of) results.
*/
int luaD_poscall(lua_State *L, CallInfo *ci, StkId firstResult, int nres) {
    StkId res;
    int wanted = ci->nresults;
    if (L->hookmask & (LUA_MASKRET | LUA_MASKLINE)) {
        if (L->hookmask & LUA_MASKRET) {
            ptrdiff_t fr = savestack(L, firstResult);  /* hook may change stack */
            luaD_hook(L, LUA_HOOKRET, -1);
            firstResult = restorestack(L, fr);
        }
        L->oldpc = ci->previous->u.l.savedpc;  /* 'oldpc' for caller function */
    }
    res = ci->func;  /* res == final position of 1st result */
    L->ci = ci->previous;  /* back to caller */
	if (L->ci_depth > 0) {
		L->ci_depth--;
	}
    /* move results to proper place */
    return moveresults(L, firstResult, res, nres, wanted);
}


/*
** Check appropriate error for stack overflow ("regular" overflow or
** overflow while handling stack overflow). If 'nCalls' is larger than
** LUAI_MAXCCALLS (which means it is handling a "regular" overflow) but
** smaller than 9/8 of LUAI_MAXCCALLS, does not report an error (to
** allow overflow handling to work)
*/
static void stackerror(lua_State *L) {
    if (L->nCcalls == LUAI_MAXCCALLS)
        luaG_runerror(L, "C stack overflow");
    else if (L->nCcalls >= (LUAI_MAXCCALLS + (LUAI_MAXCCALLS >> 3)))
        luaD_throw(L, LUA_ERRERR);  /* error while handing stack error */
}


/*
** Call a function (C or Lua). The function to be called is at *func.
** The arguments are on the stack, right after the function.
** When returns, all the results are on the stack, starting at the original
** function position.
*/
void luaD_call(lua_State *L, StkId func, int nResults) {
    if (++L->nCcalls >= LUAI_MAXCCALLS)
        stackerror(L);
    if (!luaD_precall(L, func, nResults))  /* is a Lua function? */
        luaV_execute(L);  /* call it */
	if (L->state & (lua_VMState::LVM_STATE_BREAK | lua_VMState::LVM_STATE_SUSPEND)) {
		return;
	}
    L->nCcalls--;
}


/*
** Similar to 'luaD_call', but does not allow yields during the call
*/
void luaD_callnoyield(lua_State *L, StkId func, int nResults) {
    L->nny++;
    luaD_call(L, func, nResults);
	if (L->state & (lua_VMState::LVM_STATE_BREAK| lua_VMState::LVM_STATE_SUSPEND)) {
		return;
	}
    L->nny--;
}


/*
** Completes the execution of an interrupted C function, calling its
** continuation function.
*/
static void finishCcall(lua_State *L, int status) {
    CallInfo *ci = L->ci;
    int n;
    /* must have a continuation and must be able to call it */
    lua_assert(ci->u.c.k != nullptr && L->nny == 0);
    /* error status can only happen in a protected call */
    lua_assert((ci->callstatus & CIST_YPCALL) || status == LUA_YIELD);
    if (ci->callstatus & CIST_YPCALL) {  /* was inside a pcall? */
        ci->callstatus &= ~CIST_YPCALL;  /* finish 'lua_pcall' */
        L->errfunc = ci->u.c.old_errfunc;
    }
    /* finish 'lua_callk'/'lua_pcall'; CIST_YPCALL and 'errfunc' already
       handled */
    adjustresults(L, ci->nresults);
    /* call continuation function */
    lua_unlock(L);
    n = (*ci->u.c.k)(L, status, ci->u.c.ctx);
    lua_lock(L);
    api_checknelems(L, n);
    /* finish 'luaD_precall' */
    luaD_poscall(L, ci, L->top - n, n);
}


/*
** Executes "full continuation" (everything in the stack) of a
** previously interrupted coroutine until the stack is empty (or another
** interruption long-jumps out of the loop). If the coroutine is
** recovering from an error, 'ud' points to the error status, which must
** be passed to the first continuation function (otherwise the default
** status is LUA_YIELD).
*/
static void unroll(lua_State *L, void *ud) {
    if (ud != nullptr)  /* error status? */
        finishCcall(L, *(int *)ud);  /* finish 'lua_pcallk' callee */
    while (L->ci != &L->base_ci) {  /* something in the stack */
        if (!isLua(L->ci))  /* C function? */
            finishCcall(L, LUA_YIELD);  /* complete its execution */
        else {  /* Lua function */
            luaV_finishOp(nullptr, L);  /* finish interrupted instruction */ // TODO: ctx
            luaV_execute(L);  /* execute down to higher C 'boundary' */
			if (L->state & (lua_VMState::LVM_STATE_BREAK | lua_VMState::LVM_STATE_SUSPEND)) {
				return;
			}
        }
    }
}


/*
** Try to find a suspended protected call (a "recover point") for the
** given thread.
*/
static CallInfo *findpcall(lua_State *L) {
    CallInfo *ci;
    for (ci = L->ci; ci != nullptr; ci = ci->previous) {  /* search for a pcall */
        if (ci->callstatus & CIST_YPCALL)
            return ci;
    }
    return nullptr;  /* no pending pcall */
}


/*
** Recovers from an error in a coroutine. Finds a recover point (if
** there is one) and completes the execution of the interrupted
** 'luaD_pcall'. If there is no recover point, returns zero.
*/
static int recover(lua_State *L, int status) {
    StkId oldtop;
    CallInfo *ci = findpcall(L);
    if (ci == nullptr) return 0;  /* no recovery point */
    /* "finish" luaD_pcall */
    oldtop = restorestack(L, ci->extra);
    luaF_close(L, oldtop);
    seterrorobj(L, status, oldtop);
    L->ci = ci;
    L->allowhook = getoah(ci->callstatus);  /* restore original 'allowhook' */
    L->nny = 0;  /* should be zero to be yieldable */
    luaD_shrinkstack(L);
    L->errfunc = ci->u.c.old_errfunc;
    return 1;  /* continue running the coroutine */
}


/*
** signal an error in the call to 'resume', not in the execution of the
** coroutine itself. (Such errors should not be handled by any coroutine
** error handler and should not kill the coroutine.)
*/
static void resume_error(lua_State *L, const char *msg, StkId firstArg) {
    L->top = firstArg;  /* remove args from the stack */
    setsvalue2s(L, L->top, luaS_new(L, msg));  /* push error message */
    api_incr_top(L);
    luaD_throw(L, -1);  /* jump back to 'lua_resume' */
}


/*
** Do the work for 'lua_resume' in protected mode. Most of the work
** depends on the status of the coroutine: initial state, suspended
** inside a hook, or regularly suspended (optionally with a continuation
** function), plus erroneous cases: non-suspended coroutine or dead
** coroutine.
*/
static void resume(lua_State *L, void *ud) {
    int nCcalls = L->nCcalls;
    int n = *(lua_cast(int*, ud));  /* number of arguments */
    StkId firstArg = L->top - n;  /* first argument */
    CallInfo *ci = L->ci;
    if (nCcalls >= LUAI_MAXCCALLS)
        resume_error(L, "C stack overflow", firstArg);
    if (L->status == LUA_OK) {  /* may be starting a coroutine */
        if (ci != &L->base_ci)  /* not in base level? */
            resume_error(L, "cannot resume non-suspended coroutine", firstArg);
        /* coroutine is in base level; start running it */
        if (!luaD_precall(L, firstArg - 1, LUA_MULTRET))  /* Lua function? */
            luaV_execute(L);  /* call it */
    }
    else if (L->status != LUA_YIELD)
        resume_error(L, "cannot resume dead coroutine", firstArg);
    else {  /* resuming from previous yield */
        L->status = LUA_OK;  /* mark that it is running (again) */
        ci->func = restorestack(L, ci->extra);
        if (isLua(ci))  /* yielded inside a hook? */
            luaV_execute(L);  /* just continue running Lua code */
        else {  /* 'common' yield */
            if (ci->u.c.k != nullptr) {  /* does it have a continuation function? */
                lua_unlock(L);
                n = (*ci->u.c.k)(L, LUA_YIELD, ci->u.c.ctx); /* call continuation */
                lua_lock(L);
                api_checknelems(L, n);
                firstArg = L->top - n;  /* yield results come from continuation */
            }
            luaD_poscall(L, ci, firstArg, n);  /* finish 'luaD_precall' */
        }
        unroll(L, nullptr);  /* run continuation */
    }
    lua_assert(nCcalls == L->nCcalls);
}


LUA_API int lua_resume(lua_State *L, lua_State *from, int nargs) {
    int status;
    unsigned short oldnny = L->nny;  /* save "number of non-yieldable" calls */
    lua_lock(L);
    luai_userstateresume(L, nargs);
    L->nCcalls = (from) ? from->nCcalls + 1 : 1;
    L->nny = 0;  /* allow yields */
    api_checknelems(L, (L->status == LUA_OK) ? nargs + 1 : nargs);
    status = luaD_rawrunprotected(L, resume, &nargs);
    if (status == -1)  /* error calling 'lua_resume'? */
        status = LUA_ERRRUN;
    else {  /* continue running after recoverable errors */
        while (errorstatus(status) && recover(L, status)) {
            /* unroll continuation */
            status = luaD_rawrunprotected(L, unroll, &status);
        }
        if (errorstatus(status)) {  /* unrecoverable error? */
            L->status = cast_byte(status);  /* mark thread as 'dead' */
            seterrorobj(L, status, L->top);  /* push error message */
            L->ci->top = L->top;
        }
        else lua_assert(status == L->status);  /* normal end or yield */
    }
    L->nny = oldnny;  /* restore 'nny' */
    L->nCcalls--;
    lua_assert(L->nCcalls == ((from) ? from->nCcalls : 0));
    lua_unlock(L);
    return status;
}


LUA_API int lua_isyieldable(lua_State *L) {
    return (L->nny == 0);
}


LUA_API int lua_yieldk(lua_State *L, int nresults, lua_KContext ctx,
    lua_KFunction k) {
    CallInfo *ci = L->ci;
    luai_userstateyield(L, nresults);
    lua_lock(L);
    api_checknelems(L, nresults);
    if (L->nny > 0) {
        luaG_runerror(L, "attempt to yield from outside a coroutine");
    }
    L->status = LUA_YIELD;
    ci->extra = savestack(L, ci->func);  /* save current 'func' */
    if (isLua(ci)) {  /* inside a hook? */
        api_check(L, k == nullptr, "hooks cannot continue after yielding");
    }
    else {
        if ((ci->u.c.k = k) != nullptr)  /* is there a continuation? */
            ci->u.c.ctx = ctx;  /* save context */
        ci->func = L->top - nresults - 1;  /* protect stack below results */
        luaD_throw(L, LUA_YIELD);
    }
    lua_assert(ci->callstatus & CIST_HOOKED);  /* must be inside a hook */
    lua_unlock(L);
    return 0;  /* return to 'luaD_hook' */
}


int luaD_pcall(lua_State *L, Pfunc func, void *u,
    ptrdiff_t old_top, ptrdiff_t ef) {
    int status;
    CallInfo *old_ci = L->ci;
    lu_byte old_allowhooks = L->allowhook;
    unsigned short old_nny = L->nny;
    ptrdiff_t old_errfunc = L->errfunc;
    L->errfunc = ef;
    status = luaD_rawrunprotected(L, func, u);
    if (L->force_stopping)
        status = LUA_ERRRUN;
    if (status != LUA_OK) {  /* an error occurred? */
        StkId oldtop = restorestack(L, old_top);
        luaF_close(L, oldtop);  /* close possible pending closures */
        seterrorobj(L, status, oldtop);
        L->ci = old_ci;
        L->allowhook = old_allowhooks;
        L->nny = old_nny;
        luaD_shrinkstack(L);
    }
	if (status == LUA_OK && (L->state & (lua_VMState::LVM_STATE_BREAK | lua_VMState::LVM_STATE_SUSPEND))) {
		return status;
	}
    L->errfunc = old_errfunc;
    return status;
}



/*
** Execute a protected parser.
*/
struct SParser {  /* data to 'f_parser' */
    ZIO *z;
    Mbuffer buff;  /* dynamic structure used by the scanner */
    Dyndata dyd;  /* dynamic structures used by the parser */
    const char *mode;
    const char *name;
};


static void checkmode(lua_State *L, const char *mode, const char *x) {
    if (mode && strchr(mode, x[0]) == nullptr) {
        luaO_pushfstring(L,
            "attempt to load a %s chunk (mode is '%s')", x, mode);
        luaD_throw(L, LUA_ERRSYNTAX);
    }
}


static void f_parser(lua_State *L, void *ud) {
	uvm_types::GcLClosure *cl;
    struct SParser *p = lua_cast(struct SParser *, ud);
    int c = zgetc(p->z);  /* read first character */
    if (c == LUA_SIGNATURE[0]) {
        checkmode(L, p->mode, "binary");
        cl = luaU_undump(L, p->z, p->name);
    }
    else {
        checkmode(L, p->mode, "text");
        cl = luaY_parser(L, p->z, &p->buff, &p->dyd, p->name, c);
    }
	if (!cl)
		return;
    lua_assert(cl->nupvalues == cl->p->upvalues.size());
    luaF_initupvals(L, cl);
}


int luaD_protectedparser(lua_State *L, ZIO *z, const char *name,
    const char *mode) {
    struct SParser p;
    int status;
    L->nny++;  /* cannot yield during parsing */
    p.z = z; p.name = name; p.mode = mode;
    p.dyd.actvar.arr = nullptr; p.dyd.actvar.size = 0;
    p.dyd.gt.arr = nullptr; p.dyd.gt.size = 0;
    p.dyd.label.arr = nullptr; p.dyd.label.size = 0;
    luaZ_initbuffer(L, &p.buff);
    status = luaD_pcall(L, f_parser, &p, savestack(L, L->top), L->errfunc);
    luaZ_freebuffer(L, &p.buff);
    luaM_freearray(L, p.dyd.actvar.arr, p.dyd.actvar.size);
    luaM_freearray(L, p.dyd.gt.arr, p.dyd.gt.size);
    luaM_freearray(L, p.dyd.label.arr, p.dyd.label.size);
    L->nny--;
    return status;
}




