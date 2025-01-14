/*
** $Id: lcode.c,v 2.103 2015/11/19 19:16:22 roberto Exp $
** Code generator for Lua
** See Copyright Notice in lua.h
*/

#define lcode_cpp
#define LUA_CORE

#include "uvm/lprefix.h"


#include <math.h>
#include <stdlib.h>

#include "uvm/lua.h"

#include "uvm/lcode.h"
#include "uvm/ldebug.h"
#include "uvm/ldo.h"
#include "uvm/lgc.h"
#include "uvm/llex.h"
#include "uvm/lmem.h"
#include "uvm/lobject.h"
#include "uvm/lopcodes.h"
#include "uvm/lparser.h"
#include "uvm/lstring.h"
#include "uvm/ltable.h"
#include "uvm/lvm.h"


/* Maximum number of registers in a Lua function (must fit in 8 bits) */
#define MAXREGS		255


#define hasjumps(e)	((e)->t != (e)->f)


static int tonumeral(expdesc *e, TValue *v) {
    if (hasjumps(e))
        return 0;  /* not a numeral */
    switch (e->k) {
    case VKINT:
        if (v) setivalue(v, e->u.ival);
        return 1;
    case VKFLT:
        if (v) setfltvalue(v, e->u.nval);
        return 1;
    default: return 0;
    }
}


void luaK_nil(FuncState *fs, int from, int n) {
    Instruction *previous;
    int l = from + n - 1;  /* last register to set nil */
    if (fs->pc > fs->lasttarget) {  /* no jumps to current position? */
        previous = &fs->f->codes[fs->pc - 1];
        if (GET_OPCODE(*previous) == UOP_LOADNIL) {
            int pfrom = GETARG_A(*previous);
            int pl = pfrom + GETARG_B(*previous);
            if ((pfrom <= from && from <= pl + 1) ||
                (from <= pfrom && pfrom <= l + 1)) {  /* can connect both? */
                if (pfrom < from) from = pfrom;  /* from = min(from, pfrom) */
                if (pl > l) l = pl;  /* l = max(l, pl) */
                SETARG_A(*previous, from);
                SETARG_B(*previous, l - from);
                return;
            }
        }  /* else go through */
    }
    luaK_codeABC(fs, UOP_LOADNIL, from, n - 1, 0);  /* else no optimization */
}


int luaK_jump(FuncState *fs) {
    int jpc = fs->jpc;  /* save list of jumps to here */
    int j;
    fs->jpc = NO_JUMP;
    j = luaK_codeAsBx(fs, UOP_JMP, 0, NO_JUMP);
    luaK_concat(fs, &j, jpc);  /* keep them on hold */
    return j;
}


void luaK_ret(FuncState *fs, int first, int nret) {
    luaK_codeABC(fs, UOP_RETURN, first, nret + 1, 0);
}


static int condjump(FuncState *fs, OpCode op, int A, int B, int C) {
    luaK_codeABC(fs, op, A, B, C);
    return luaK_jump(fs);
}


static void fixjump(FuncState *fs, int pc, int dest) {
    Instruction *jmp = &fs->f->codes[pc];
    int offset = dest - (pc + 1);
    lua_assert(dest != NO_JUMP);
    if (abs(offset) > MAXARG_sBx)
        luaX_syntaxerror(fs->ls, "control structure too long");
    SETARG_sBx(*jmp, offset);
}


/*
** returns current 'pc' and marks it as a jump target (to avoid wrong
** optimizations with consecutive instructions not in the same basic block).
*/
int luaK_getlabel(FuncState *fs) {
    fs->lasttarget = fs->pc;
    return fs->pc;
}


static int getjump(FuncState *fs, int pc) {
    int offset = GETARG_sBx(fs->f->codes[pc]);
    if (offset == NO_JUMP)  /* point to itself represents end of list */
        return NO_JUMP;  /* end of list */
    else
        return (pc + 1) + offset;  /* turn offset into absolute position */
}


static Instruction *getjumpcontrol(FuncState *fs, int pc) {
    Instruction *pi = &fs->f->codes[pc];
    if (pc >= 1 && testTMode(GET_OPCODE(*(pi - 1))))
        return pi - 1;
    else
        return pi;
}


/*
** check whether list has any jump that do not produce a value
** (or produce an inverted value)
*/
static int need_value(FuncState *fs, int list) {
    for (; list != NO_JUMP; list = getjump(fs, list)) {
        Instruction i = *getjumpcontrol(fs, list);
        if (GET_OPCODE(i) != UOP_TESTSET) return 1;
    }
    return 0;  /* not found */
}


static int patchtestreg(FuncState *fs, int node, int reg) {
    Instruction *i = getjumpcontrol(fs, node);
    if (GET_OPCODE(*i) != UOP_TESTSET)
        return 0;  /* cannot patch other instructions */
    if (reg != NO_REG && reg != GETARG_B(*i))
        SETARG_A(*i, reg);
    else  /* no register to put value or register already has the value */
        *i = CREATE_ABC(UOP_TEST, GETARG_B(*i), 0, GETARG_C(*i));

    return 1;
}


static void removevalues(FuncState *fs, int list) {
    for (; list != NO_JUMP; list = getjump(fs, list))
        patchtestreg(fs, list, NO_REG);
}


static void patchlistaux(FuncState *fs, int list, int vtarget, int reg,
    int dtarget) {
    while (list != NO_JUMP) {
        int next = getjump(fs, list);
        if (patchtestreg(fs, list, reg))
            fixjump(fs, list, vtarget);
        else
            fixjump(fs, list, dtarget);  /* jump to default target */
        list = next;
    }
}


static void dischargejpc(FuncState *fs) {
    patchlistaux(fs, fs->jpc, fs->pc, NO_REG, fs->pc);
    fs->jpc = NO_JUMP;
}


void luaK_patchlist(FuncState *fs, int list, int target) {
    if (target == fs->pc)
        luaK_patchtohere(fs, list);
    else {
        lua_assert(target < fs->pc);
        patchlistaux(fs, list, target, NO_REG, target);
    }
}


void luaK_patchclose(FuncState *fs, int list, int level) {
    level++;  /* argument is +1 to reserve 0 as non-op */
    while (list != NO_JUMP) {
        int next = getjump(fs, list);
        lua_assert(list < fs->f->codes.size() && GET_OPCODE(fs->f->codes[list]) == UOP_JMP &&
            (GETARG_A(fs->f->codes[list]) == 0 ||
            GETARG_A(fs->f->codes[list]) >= level));
        SETARG_A(fs->f->codes[list], level);
        list = next;
    }
}


void luaK_patchtohere(FuncState *fs, int list) {
    luaK_getlabel(fs);
    luaK_concat(fs, &fs->jpc, list);
}


void luaK_concat(FuncState *fs, int *l1, int l2) {
    if (l2 == NO_JUMP) return;
    else if (*l1 == NO_JUMP)
        *l1 = l2;
    else {
        int list = *l1;
        int next;
        while ((next = getjump(fs, list)) != NO_JUMP)  /* find last element */
            list = next;
        fixjump(fs, list, l2);
    }
}


static int luaK_code(FuncState *fs, Instruction i) {
	uvm_types::GcProto *f = fs->f;
    dischargejpc(fs);  /* 'pc' will change */
    /* put new instruction in code array */
	if (size_t(fs->pc) > f->codes.size()) {
		auto oldsize = f->codes.size();
		f->codes.resize(fs->pc);
		memset(f->codes.data() + oldsize, 0x0, sizeof(f->codes[0]) * (fs->pc-oldsize));
	}
	if (size_t(fs->pc) == f->codes.size()) {
		f->codes.resize(fs->pc + 1 );
	}
    f->codes[fs->pc] = i;
    /* save corresponding line information */
	if (size_t(fs->pc) > f->lineinfos.size()) {
		auto oldsize = f->lineinfos.size();
		f->lineinfos.resize(fs->pc);
		memset(f->lineinfos.data() + oldsize, 0x0, sizeof(f->lineinfos[0]) * (fs->pc - oldsize));
	}

	if (size_t(fs->pc) == f->lineinfos.size()) {
		f->lineinfos.resize(fs->pc + 1);
	}
    f->lineinfos[fs->pc] = fs->ls->lastline;
    return fs->pc++;
}


int luaK_codeABC(FuncState *fs, OpCode o, int a, int b, int c) {
    lua_assert(getOpMode(o) == iABC);
    lua_assert(getBMode(o) != OpArgN || b == 0);
    lua_assert(getCMode(o) != OpArgN || c == 0);
    lua_assert(a <= MAXARG_A && b <= MAXARG_B && c <= MAXARG_C);
    return luaK_code(fs, CREATE_ABC(o, a, b, c));
}


int luaK_codeABx(FuncState *fs, OpCode o, int a, unsigned int bc) {
    lua_assert(getOpMode(o) == iABx || getOpMode(o) == iAsBx);
    lua_assert(getCMode(o) == OpArgN);
    lua_assert(a <= MAXARG_A && bc <= MAXARG_Bx);
    return luaK_code(fs, CREATE_ABx(o, a, bc));
}


static int codeextraarg(FuncState *fs, int a) {
    lua_assert(a <= MAXARG_Ax);
    return luaK_code(fs, CREATE_Ax(UOP_EXTRAARG, a));
}


int luaK_codek(FuncState *fs, int reg, int k) {
    if (k <= MAXARG_Bx)
        return luaK_codeABx(fs, UOP_LOADK, reg, k);
    else {
        int p = luaK_codeABx(fs, UOP_LOADKX, reg, 0);
        codeextraarg(fs, k);
        return p;
    }
}


void luaK_checkstack(FuncState *fs, int n) {
    int newstack = fs->freereg + n;
    if (newstack > fs->f->maxstacksize) {
        if (newstack >= MAXREGS)
            luaX_syntaxerror(fs->ls,
            "function or expression needs too many registers");
        fs->f->maxstacksize = cast_byte(newstack);
    }
}


void luaK_reserveregs(FuncState *fs, int n) {
    luaK_checkstack(fs, n);
    fs->freereg += n;
}


static void freereg(FuncState *fs, int reg) {
    if (!ISK(reg) && reg >= fs->nactvar) {
        fs->freereg--;
        lua_assert(reg == fs->freereg);
    }
}


static void freeexp(FuncState *fs, expdesc *e) {
    if (e->k == VNONRELOC)
        freereg(fs, e->u.info);
}


/*
** Use scanner's table to cache position of constants in constant list
** and try to reuse constants
*/
static int addk(FuncState *fs, TValue *key, TValue *v) {
    lua_State *L = fs->ls->L;
	uvm_types::GcProto *f = fs->f;
    TValue *idx = luaH_set(L, fs->ls->h, key, true);  /* index scanner table */
	int k;
	size_t oldsize;
    if (ttisinteger(idx)) {  /* is there an index there? */
        k = cast_int(ivalue(idx));
        /* correct value? (warning: must distinguish floats from integers!) */
        if (k < fs->nk && ttype(&f->ks[k]) == ttype(v) &&
            luaV_rawequalobj(&f->ks[k], v))
            return k;  /* reuse index */
    }
    /* constant not found; create a new entry */
    oldsize = f->ks.size();
    k = fs->nk;
    /* numerical value does not need GC barrier;
       table has no metatable, so it does not need to invalidate cache */
    setivalue(idx, k);
	if (size_t(k) > f->ks.size()) {
		auto oldsize = f->ks.size();
		f->ks.resize(k);
		memset(f->ks.data() + oldsize, 0x0, sizeof(f->ks[0]) * (k - oldsize));
	}
    while (oldsize < f->ks.size()) setnilvalue(&f->ks[oldsize++]);
	if (f->ks.size() <= size_t(k))
		f->ks.resize(k + 1);
    setobj(L, &f->ks[k], v);
    fs->nk++;
    return k;
}


int luaK_stringK(FuncState *fs, uvm_types::GcString *s) {
    TValue o;
    setsvalue(fs->ls->L, &o, s);
    return addk(fs, &o, &o);
}


/*
** Integers use userdata as keys to avoid collision with floats with same
** value; conversion to 'void*' used only for hashing, no "precision"
** problems
*/
int luaK_intK(FuncState *fs, lua_Integer n) {
    TValue k, o;
    setpvalue(&k, lua_cast(void*, lua_cast(size_t, n)));
    setivalue(&o, n);
    return addk(fs, &k, &o);
}


static int luaK_numberK(FuncState *fs, lua_Number r) {
    TValue o;
    setfltvalue(&o, r);
    return addk(fs, &o, &o);
}


static int boolK(FuncState *fs, int b) {
    TValue o;
    setbvalue(&o, b);
    return addk(fs, &o, &o);
}


static int nilK(FuncState *fs) {
    TValue k, v;
    setnilvalue(&v);
    /* cannot use nil as key; instead use table itself to represent nil */
    sethvalue(fs->ls->L, &k, fs->ls->h);
    return addk(fs, &k, &v);
}


void luaK_setreturns(FuncState *fs, expdesc *e, int nresults) {
    if (e->k == VCALL) {  /* expression is an open function call? */
        SETARG_C(getcode(fs, e), nresults + 1);
    }
    else if (e->k == VVARARG) {
        SETARG_B(getcode(fs, e), nresults + 1);
        SETARG_A(getcode(fs, e), fs->freereg);
        luaK_reserveregs(fs, 1);
    }
}


void luaK_setoneret(FuncState *fs, expdesc *e) {
    if (e->k == VCALL) {  /* expression is an open function call? */
        e->k = VNONRELOC;
        e->u.info = GETARG_A(getcode(fs, e));
    }
    else if (e->k == VVARARG) {
        SETARG_B(getcode(fs, e), 2);
        e->k = VRELOCABLE;  /* can relocate its simple result */
    }
}


void luaK_dischargevars(FuncState *fs, expdesc *e) {
    switch (e->k) {
    case VLOCAL: {
        e->k = VNONRELOC;
        break;
    }
    case VUPVAL: {
        e->u.info = luaK_codeABC(fs, UOP_GETUPVAL, 0, e->u.info, 0);
        e->k = VRELOCABLE;
        break;
    }
    case VINDEXED: {
        OpCode op = UOP_GETTABUP;  /* assume 't' is in an upvalue */
        freereg(fs, e->u.ind.idx);
        if (e->u.ind.vt == VLOCAL) {  /* 't' is in a register? */
            freereg(fs, e->u.ind.t);
            op = UOP_GETTABLE;
        }
        e->u.info = luaK_codeABC(fs, op, 0, e->u.ind.t, e->u.ind.idx);
        e->k = VRELOCABLE;
        break;
    }
    case VVARARG:
    case VCALL: {
        luaK_setoneret(fs, e);
        break;
    }
    default: break;  /* there is one value available (somewhere) */
    }
}


static int code_label(FuncState *fs, int A, int b, int jump) {
    luaK_getlabel(fs);  /* those instructions may be jump targets */
    return luaK_codeABC(fs, UOP_LOADBOOL, A, b, jump);
}


static void discharge2reg(FuncState *fs, expdesc *e, int reg) {
    luaK_dischargevars(fs, e);
    switch (e->k) {
    case VNIL: {
        luaK_nil(fs, reg, 1);
        break;
    }
    case VFALSE: case VTRUE: {
        luaK_codeABC(fs, UOP_LOADBOOL, reg, e->k == VTRUE, 0);
        break;
    }
    case VK: {
        luaK_codek(fs, reg, e->u.info);
        break;
    }
    case VKFLT: {
        luaK_codek(fs, reg, luaK_numberK(fs, e->u.nval));
        break;
    }
    case VKINT: {
        luaK_codek(fs, reg, luaK_intK(fs, e->u.ival));
        break;
    }
    case VRELOCABLE: {
        Instruction *pc = &getcode(fs, e);
        SETARG_A(*pc, reg);
        break;
    }
    case VNONRELOC: {
        if (reg != e->u.info)
            luaK_codeABC(fs, UOP_MOVE, reg, e->u.info, 0);
        break;
    }
    default: {
        lua_assert(e->k == VVOID || e->k == VJMP);
        return;  /* nothing to do... */
    }
    }
    e->u.info = reg;
    e->k = VNONRELOC;
}


static void discharge2anyreg(FuncState *fs, expdesc *e) {
    if (e->k != VNONRELOC) {
        luaK_reserveregs(fs, 1);
        discharge2reg(fs, e, fs->freereg - 1);
    }
}


static void exp2reg(FuncState *fs, expdesc *e, int reg) {
    discharge2reg(fs, e, reg);
    if (e->k == VJMP)
        luaK_concat(fs, &e->t, e->u.info);  /* put this jump in 't' list */
    if (hasjumps(e)) {
        int final;  /* position after whole expression */
        int p_f = NO_JUMP;  /* position of an eventual LOAD false */
        int p_t = NO_JUMP;  /* position of an eventual LOAD true */
        if (need_value(fs, e->t) || need_value(fs, e->f)) {
            int fj = (e->k == VJMP) ? NO_JUMP : luaK_jump(fs);
            p_f = code_label(fs, reg, 0, 1);
            p_t = code_label(fs, reg, 1, 0);
            luaK_patchtohere(fs, fj);
        }
        final = luaK_getlabel(fs);
        patchlistaux(fs, e->f, final, reg, p_f);
        patchlistaux(fs, e->t, final, reg, p_t);
    }
    e->f = e->t = NO_JUMP;
    e->u.info = reg;
    e->k = VNONRELOC;
}


void luaK_exp2nextreg(FuncState *fs, expdesc *e) {
    luaK_dischargevars(fs, e);
    freeexp(fs, e);
    luaK_reserveregs(fs, 1);
    exp2reg(fs, e, fs->freereg - 1);
}


int luaK_exp2anyreg(FuncState *fs, expdesc *e) {
    luaK_dischargevars(fs, e);
    if (e->k == VNONRELOC) {
        if (!hasjumps(e)) return e->u.info;  /* exp is already in a register */
        if (e->u.info >= fs->nactvar) {  /* reg. is not a local? */
            exp2reg(fs, e, e->u.info);  /* put value on it */
            return e->u.info;
        }
    }
    luaK_exp2nextreg(fs, e);  /* default */
    return e->u.info;
}


void luaK_exp2anyregup(FuncState *fs, expdesc *e) {
    if (e->k != VUPVAL || hasjumps(e))
        luaK_exp2anyreg(fs, e);
}


void luaK_exp2val(FuncState *fs, expdesc *e) {
    if (hasjumps(e))
        luaK_exp2anyreg(fs, e);
    else
        luaK_dischargevars(fs, e);
}


int luaK_exp2RK(FuncState *fs, expdesc *e) {
    luaK_exp2val(fs, e);
    switch (e->k) {
    case VTRUE:
    case VFALSE:
    case VNIL: {
        if (fs->nk <= MAXINDEXRK) {  /* constant fits in RK operand? */
            e->u.info = (e->k == VNIL) ? nilK(fs) : boolK(fs, (e->k == VTRUE));
            e->k = VK;
            return RKASK(e->u.info);
        }
        else break;
    }
    case VKINT: {
        e->u.info = luaK_intK(fs, e->u.ival);
        e->k = VK;
        goto vk;
    }
    case VKFLT: {
        e->u.info = luaK_numberK(fs, e->u.nval);
        e->k = VK;
    }
                /* FALLTHROUGH */
    case VK: {
    vk:
        if (e->u.info <= MAXINDEXRK)  /* constant fits in 'argC'? */
            return RKASK(e->u.info);
        else break;
    }
    default: break;
    }
    /* not a constant in the right range: put it in a register */
    return luaK_exp2anyreg(fs, e);
}


void luaK_storevar(FuncState *fs, expdesc *var, expdesc *ex) {
    switch (var->k) {
    case VLOCAL: {
        freeexp(fs, ex);
        exp2reg(fs, ex, var->u.info);
        return;
    }
    case VUPVAL: {
        int e = luaK_exp2anyreg(fs, ex);
        luaK_codeABC(fs, UOP_SETUPVAL, e, var->u.info, 0);
        break;
    }
    case VINDEXED: {
        OpCode op = (var->u.ind.vt == VLOCAL) ? UOP_SETTABLE : UOP_SETTABUP;
        int e = luaK_exp2RK(fs, ex);
        luaK_codeABC(fs, op, var->u.ind.t, var->u.ind.idx, e);
        break;
    }
    default: {
        lua_assert(0);  /* invalid var kind to store */
        break;
    }
    }
    freeexp(fs, ex);
}


void luaK_self(FuncState *fs, expdesc *e, expdesc *key) {
    int ereg;
    luaK_exp2anyreg(fs, e);
    ereg = e->u.info;  /* register where 'e' was placed */
    freeexp(fs, e);
    e->u.info = fs->freereg;  /* base register for op_self */
    e->k = VNONRELOC;
    luaK_reserveregs(fs, 2);  /* function and 'self' produced by op_self */
    luaK_codeABC(fs, UOP_SELF, e->u.info, ereg, luaK_exp2RK(fs, key));
    freeexp(fs, key);
}


static void invertjump(FuncState *fs, expdesc *e) {
    Instruction *pc = getjumpcontrol(fs, e->u.info);
    lua_assert(testTMode(GET_OPCODE(*pc)) && GET_OPCODE(*pc) != UOP_TESTSET &&
        GET_OPCODE(*pc) != UOP_TEST);
    SETARG_A(*pc, !(GETARG_A(*pc)));
}


static int jumponcond(FuncState *fs, expdesc *e, int cond) {
    if (e->k == VRELOCABLE) {
        Instruction ie = getcode(fs, e);
        if (GET_OPCODE(ie) == UOP_NOT) {
            fs->pc--;  /* remove previous UOP_NOT */
            return condjump(fs, UOP_TEST, GETARG_B(ie), 0, !cond);
        }
        /* else go through */
    }
    discharge2anyreg(fs, e);
    freeexp(fs, e);
    return condjump(fs, UOP_TESTSET, NO_REG, e->u.info, cond);
}


void luaK_goiftrue(FuncState *fs, expdesc *e) {
    int pc;  /* pc of last jump */
    luaK_dischargevars(fs, e);
    switch (e->k) {
    case VJMP: {
        invertjump(fs, e);
        pc = e->u.info;
        break;
    }
    case VK: case VKFLT: case VKINT: case VTRUE: {
        pc = NO_JUMP;  /* always true; do nothing */
        break;
    }
    default: {
        pc = jumponcond(fs, e, 0);
        break;
    }
    }
    luaK_concat(fs, &e->f, pc);  /* insert last jump in 'f' list */
    luaK_patchtohere(fs, e->t);
    e->t = NO_JUMP;
}


void luaK_goiffalse(FuncState *fs, expdesc *e) {
    int pc;  /* pc of last jump */
    luaK_dischargevars(fs, e);
    switch (e->k) {
    case VJMP: {
        pc = e->u.info;
        break;
    }
    case VNIL: case VFALSE: {
        pc = NO_JUMP;  /* always false; do nothing */
        break;
    }
    default: {
        pc = jumponcond(fs, e, 1);
        break;
    }
    }
    luaK_concat(fs, &e->t, pc);  /* insert last jump in 't' list */
    luaK_patchtohere(fs, e->f);
    e->f = NO_JUMP;
}


static void codenot(FuncState *fs, expdesc *e) {
    luaK_dischargevars(fs, e);
    switch (e->k) {
    case VNIL: case VFALSE: {
        e->k = VTRUE;
        break;
    }
    case VK: case VKFLT: case VKINT: case VTRUE: {
        e->k = VFALSE;
        break;
    }
    case VJMP: {
        invertjump(fs, e);
        break;
    }
    case VRELOCABLE:
    case VNONRELOC: {
        discharge2anyreg(fs, e);
        freeexp(fs, e);
        e->u.info = luaK_codeABC(fs, UOP_NOT, 0, e->u.info, 0);
        e->k = VRELOCABLE;
        break;
    }
    default: {
        lua_assert(0);  /* cannot happen */
        break;
    }
    }
    /* interchange true and false lists */
  { int temp = e->f; e->f = e->t; e->t = temp; }
  removevalues(fs, e->f);
  removevalues(fs, e->t);
}


void luaK_indexed(FuncState *fs, expdesc *t, expdesc *k) {
    lua_assert(!hasjumps(t));
    t->u.ind.t = t->u.info;
    t->u.ind.idx = luaK_exp2RK(fs, k);
    t->u.ind.vt = (t->k == VUPVAL) ? VUPVAL
        : check_exp(vkisinreg(t->k), VLOCAL);
    t->k = VINDEXED;
}


/*
** return false if folding can raise an error
*/
static int validop(int op, TValue *v1, TValue *v2) {
    switch (op) {
    case LUA_OPBAND: case LUA_OPBOR: case LUA_OPBXOR:
    case LUA_OPSHL: case LUA_OPSHR: case LUA_OPBNOT: {  /* conversion errors */
        lua_Integer i;
        return (tointeger(v1, &i) && tointeger(v2, &i));
    }
    case LUA_OPDIV: case LUA_OPIDIV: case LUA_OPMOD:  /* division by 0 */
        return (nvalue(v2) != 0);
    default: return 1;  /* everything else is valid */
    }
}


/*
** Try to "constant-fold" an operation; return 1 iff successful
*/
static int constfolding(FuncState *fs, int op, expdesc *e1, expdesc *e2) {
    TValue v1, v2, res;
    if (!tonumeral(e1, &v1) || !tonumeral(e2, &v2) || !validop(op, &v1, &v2))
        return 0;  /* non-numeric operands or not safe to fold */
    luaO_arith(fs->ls->L, op, &v1, &v2, &res);  /* does operation */
    if (ttisinteger(&res)) {
        e1->k = VKINT;
        e1->u.ival = ivalue(&res);
    }
    else {  /* folds neither NaN nor 0.0 (to avoid collapsing with -0.0) */
        lua_Number n = fltvalue(&res);
        if (luai_numisnan(n) || n == 0)
            return 0;
        e1->k = VKFLT;
        e1->u.nval = n;
    }
    return 1;
}


/*
** Code for binary and unary expressions that "produce values"
** (arithmetic operations, bitwise operations, concat, length). First
** try to do constant folding (only for numeric [arithmetic and
** bitwise] operations, which is what 'lua_arith' accepts).
** Expression to produce final result will be encoded in 'e1'.
*/
static void codeexpval(FuncState *fs, OpCode op,
    expdesc *e1, expdesc *e2, int line) {
    lua_assert(op >= UOP_ADD);
    if (op <= UOP_BNOT && constfolding(fs, (op - UOP_ADD) + LUA_OPADD, e1, e2))
        return;  /* result has been folded */
    else {
        int o1, o2;
        /* move operands to registers (if needed) */
        if (op == UOP_UNM || op == UOP_BNOT || op == UOP_LEN) {  /* unary op? */
            o2 = 0;  /* no second expression */
            o1 = luaK_exp2anyreg(fs, e1);  /* cannot operate on constants */
        }
        else {  /* regular case (binary operators) */
            o2 = luaK_exp2RK(fs, e2);  /* both operands are "RK" */
            o1 = luaK_exp2RK(fs, e1);
        }
        if (o1 > o2) {  /* free registers in proper order */
            freeexp(fs, e1);
            freeexp(fs, e2);
        }
        else {
            freeexp(fs, e2);
            freeexp(fs, e1);
        }
        e1->u.info = luaK_codeABC(fs, op, 0, o1, o2);  /* generate opcode */
        e1->k = VRELOCABLE;  /* all those operations are relocatable */
        luaK_fixline(fs, line);
    }
}


static void codecomp(FuncState *fs, OpCode op, int cond, expdesc *e1,
    expdesc *e2) {
    int o1 = luaK_exp2RK(fs, e1);
    int o2 = luaK_exp2RK(fs, e2);
    freeexp(fs, e2);
    freeexp(fs, e1);
    if (cond == 0 && op != UOP_EQ) {
        int temp;  /* exchange args to replace by '<' or '<=' */
        temp = o1; o1 = o2; o2 = temp;  /* o1 <==> o2 */
        cond = 1;
    }
    e1->u.info = condjump(fs, op, cond, o1, o2);
    e1->k = VJMP;
}


void luaK_prefix(FuncState *fs, UnOpr op, expdesc *e, int line) {
    expdesc e2;
    e2.t = e2.f = NO_JUMP; e2.k = VKINT; e2.u.ival = 0;
    switch (op) {
    case OPR_MINUS: case OPR_BNOT: case OPR_LEN: {
        codeexpval(fs, lua_cast(OpCode, (op - OPR_MINUS) + UOP_UNM), e, &e2, line);
        break;
    }
    case OPR_NOT: codenot(fs, e); break;
    default: lua_assert(0);
    }
}


void luaK_infix(FuncState *fs, BinOpr op, expdesc *v) {
    switch (op) {
    case OPR_AND: {
        luaK_goiftrue(fs, v);
        break;
    }
    case OPR_OR: {
        luaK_goiffalse(fs, v);
        break;
    }
    case OPR_CONCAT: {
        luaK_exp2nextreg(fs, v);  /* operand must be on the 'stack' */
        break;
    }
    case OPR_ADD: case OPR_SUB:
    case OPR_MUL: case OPR_DIV: case OPR_IDIV:
    case OPR_MOD: case OPR_POW:
    case OPR_BAND: case OPR_BOR: case OPR_BXOR:
    case OPR_SHL: case OPR_SHR: {
        if (!tonumeral(v, nullptr)) luaK_exp2RK(fs, v);
        break;
    }
    default: {
        luaK_exp2RK(fs, v);
        break;
    }
    }
}


void luaK_posfix(FuncState *fs, BinOpr op,
    expdesc *e1, expdesc *e2, int line) {
    switch (op) {
    case OPR_AND: {
        lua_assert(e1->t == NO_JUMP);  /* list must be closed */
        luaK_dischargevars(fs, e2);
        luaK_concat(fs, &e2->f, e1->f);
        *e1 = *e2;
        break;
    }
    case OPR_OR: {
        lua_assert(e1->f == NO_JUMP);  /* list must be closed */
        luaK_dischargevars(fs, e2);
        luaK_concat(fs, &e2->t, e1->t);
        *e1 = *e2;
        break;
    }
    case OPR_CONCAT: {
        luaK_exp2val(fs, e2);
        if (e2->k == VRELOCABLE && GET_OPCODE(getcode(fs, e2)) == UOP_CONCAT) {
            lua_assert(e1->u.info == GETARG_B(getcode(fs, e2)) - 1);
            freeexp(fs, e1);
            SETARG_B(getcode(fs, e2), e1->u.info);
            e1->k = VRELOCABLE; e1->u.info = e2->u.info;
        }
        else {
            luaK_exp2nextreg(fs, e2);  /* operand must be on the 'stack' */
            codeexpval(fs, UOP_CONCAT, e1, e2, line);
        }
        break;
    }
    case OPR_ADD: case OPR_SUB: case OPR_MUL: case OPR_DIV:
    case OPR_IDIV: case OPR_MOD: case OPR_POW:
    case OPR_BAND: case OPR_BOR: case OPR_BXOR:
    case OPR_SHL: case OPR_SHR: {
        codeexpval(fs, lua_cast(OpCode, (op - OPR_ADD) + UOP_ADD), e1, e2, line);
        break;
    }
    case OPR_EQ: case OPR_LT: case OPR_LE: {
        codecomp(fs, lua_cast(OpCode, (op - OPR_EQ) + UOP_EQ), 1, e1, e2);
        break;
    }
    case OPR_NE: case OPR_GT: case OPR_GE: {
        codecomp(fs, lua_cast(OpCode, (op - OPR_NE) + UOP_EQ), 0, e1, e2);
        break;
    }
    default: lua_assert(0);
    }
}


void luaK_fixline(FuncState *fs, int line) {
    fs->f->lineinfos[fs->pc - 1] = line;
}


void luaK_setlist(FuncState *fs, int base, int nelems, int tostore) {
    int c = (nelems - 1) / LFIELDS_PER_FLUSH + 1;
    int b = (tostore == LUA_MULTRET) ? 0 : tostore;
    lua_assert(tostore != 0);
    if (c <= MAXARG_C)
        luaK_codeABC(fs, UOP_SETLIST, base, b, c);
    else if (c <= MAXARG_Ax) {
        luaK_codeABC(fs, UOP_SETLIST, base, b, 0);
        codeextraarg(fs, c);
    }
    else
        luaX_syntaxerror(fs->ls, "constructor too long");
    fs->freereg = base + 1;  /* free registers with list values */
}

