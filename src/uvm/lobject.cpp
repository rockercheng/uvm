/*
** $Id: lobject.c,v 2.108 2015/11/02 16:09:30 roberto Exp $
** Some generic functions over Lua objects
** See Copyright Notice in lua.h
*/

#define lobject_cpp
#define LUA_CORE

#include "uvm/lprefix.h"


#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uvm/lua.h"

#include "uvm/lctype.h"
#include "uvm/ldebug.h"
#include "uvm/ldo.h"
#include "uvm/lmem.h"
#include "uvm/lobject.h"
#include "uvm/lstate.h"
#include "uvm/lstring.h"
#include "uvm/lvm.h"



LUAI_DDEF const TValue luaO_nilobject_ = { NILCONSTANT };


/*
** converts an integer to a "floating point byte", represented as
** (eeeeexxx), where the real value is (1xxx) * 2^(eeeee - 1) if
** eeeee != 0 and (xxx) otherwise.
*/
int luaO_int2fb(unsigned int x) {
    int e = 0;  /* exponent */
    if (x < 8) return x;
    while (x >= (8 << 4)) {  /* coarse steps */
        x = (x + 0xf) >> 4;  /* x = ceil(x / 16) */
        e += 4;
    }
    while (x >= (8 << 1)) {  /* fine steps */
        x = (x + 1) >> 1;  /* x = ceil(x / 2) */
        e++;
    }
    return ((e + 1) << 3) | (cast_int(x) - 8);
}


/* converts back */
int luaO_fb2int(int x) {
    return (x < 8) ? x : ((x & 7) + 8) << ((x >> 3) - 1);
}


/*
** Computes ceil(log2(x))
*/
int luaO_ceillog2(unsigned int x) {
    static const lu_byte log_2[256] = {  /* log_2[i] = ceil(log2(i - 1)) */
        0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
        6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8
    };
    int l = 0;
    x--;
    while (x >= 256) { l += 8; x >>= 8; }
    return l + log_2[x];
}


static lua_Integer intarith(lua_State *L, int op, lua_Integer v1,
    lua_Integer v2) {
    switch (op) {
    case LUA_OPADD: return intop(+, v1, v2);
    case LUA_OPSUB:return intop(-, v1, v2);
    case LUA_OPMUL:return intop(*, v1, v2);
    case LUA_OPMOD: return luaV_mod(L, v1, v2);
    case LUA_OPIDIV: return luaV_div(L, v1, v2);
    case LUA_OPBAND: return intop(&, v1, v2);
    case LUA_OPBOR: return intop(| , v1, v2);
    case LUA_OPBXOR: return intop(^, v1, v2);
    case LUA_OPSHL: return luaV_shiftl(v1, v2);
    case LUA_OPSHR: return luaV_shiftl(v1, -v2);
    case LUA_OPUNM: return intop(-, 0, v1);
    case LUA_OPBNOT: return intop(^, ~l_castS2U(0), v1);
    default: lua_assert(0); return 0;
    }
}


static lua_Number numarith(lua_State *L, int op, lua_Number v1,
    lua_Number v2) {
    switch (op) {
    case LUA_OPADD: return safe_number_add(v1, v2);
    case LUA_OPSUB: return safe_number_minus(v1, v2);
    case LUA_OPMUL: return safe_number_multiply(v1, v2);
    case LUA_OPDIV: return safe_number_div(v1, v2);
	case LUA_OPPOW: {
		// pow only accept integers
		auto v1_num = safe_number_to_int64(v1);
		auto v2_num = safe_number_to_int64(v2);
		return safe_number_create(std::pow(v1_num, v2_num));
	}
    case LUA_OPIDIV: return safe_number_idiv(v1, v2);
    case LUA_OPUNM: return safe_number_neg(v1);
    case LUA_OPMOD: {
		auto v1_num = safe_number_to_int64(v1);
		auto v2_num = safe_number_to_int64(v2);
		auto m_num = fmod(v1_num, v2_num);
		lua_Number m = safe_number_create(std::to_string(m_num));
        return m;
    }
    default: lua_assert(0); return safe_number_zero();
    }
}


void luaO_arith(lua_State *L, int op, const TValue *p1, const TValue *p2,
    TValue *res) {
    switch (op) {
    case LUA_OPBAND: case LUA_OPBOR: case LUA_OPBXOR:
    case LUA_OPSHL: case LUA_OPSHR:
    case LUA_OPBNOT: {  /* operate only on integers */
        lua_Integer i1; lua_Integer i2;
        if (tointeger(p1, &i1) && tointeger(p2, &i2)) {
            setivalue(res, intarith(L, op, i1, i2));
            return;
        }
        else break;  /* go to the end */
    }
    case LUA_OPDIV: case LUA_OPPOW: {  /* operate only on floats */
        lua_Number n1; lua_Number n2;
        if (tonumber(p1, &n1) && tonumber(p2, &n2)) {
            setfltvalue(res, numarith(L, op, n1, n2));
            return;
        }
        else break;  /* go to the end */
    }
    default: {  /* other operations */
        lua_Number n1; lua_Number n2;
        if (ttisinteger(p1) && ttisinteger(p2)) {
            setivalue(res, intarith(L, op, ivalue(p1), ivalue(p2)));
            return;
        }
        else if (tonumber(p1, &n1) && tonumber(p2, &n2)) {
            setfltvalue(res, numarith(L, op, n1, n2));
            return;
        }
        else break;  /* go to the end */
    }
    }
    /* could not perform raw operation; try metamethod */
    lua_assert(L != nullptr);  /* should not fail when folding (compile time) */
    luaT_trybinTM(L, p1, p2, res, lua_cast(TMS, (op - LUA_OPADD) + TM_ADD));
}


int luaO_hexavalue(int c) {
    if (lisdigit(c)) return c - '0';
    else return (ltolower(c) - 'a') + 10;
}


static int isneg(const char **s) {
    if (**s == '-') { (*s)++; return 1; }
    else if (**s == '+') (*s)++;
    return 0;
}



/*
** {==================================================================
** Lua's implementation for 'lua_strx2number'
** ===================================================================
*/

#if !defined(lua_strx2number)

/* maximum number of significant digits to read (to avoid overflows
   even with single floats) */
#define MAXSIGDIG	30

///*
//** convert an hexadecimal numeric string to a number, following
//** C99 specification for 'strtod'
//*/
//static lua_Number lua_strx2number(const char *s, char **endptr) {
//    int dot = lua_getlocaledecpoint();
//    lua_Number r = safe_number_zero();  /* result (accumulator) */
//    int sigdig = 0;  /* number of significant digits */
//    int nosigdig = 0;  /* number of non-significant digits */
//    int e = 0;  /* exponent correction */
//    int neg;  /* 1 if number is negative */
//    int hasdot = 0;  /* true after seen a dot */
//    *endptr = lua_cast(char *, s);  /* nothing is valid yet */
//    while (lisspace(cast_uchar(*s))) s++;  /* skip initial spaces */
//    neg = isneg(&s);  /* check signal */
//    if (!(*s == '0' && (*(s + 1) == 'x' || *(s + 1) == 'X')))  /* check '0x' */
//        return safe_number_zero();  /* invalid format (no '0x') */
//    for (s += 2;; s++) {  /* skip '0x' and read numeral */
//        if (*s == dot) {
//            if (hasdot) break;  /* second dot? stop loop */
//            else hasdot = 1;
//        }
//        else if (lisxdigit(cast_uchar(*s))) {
//            if (sigdig == 0 && *s == '0')  /* non-significant digit (zero)? */
//                nosigdig++;
//            else if (++sigdig <= MAXSIGDIG)  /* can read it without overflow? */
//                r = safe_number_add((safe_number_multiply(r, cast_num(safe_number_create(16)))), safe_number_create(luaO_hexavalue(*s)));
//            else e++; /* too many digits; ignore, but still count for exponent */
//            if (hasdot) e--;  /* decimal digit? correct exponent */
//        }
//        else break;  /* neither a dot nor a digit */
//    }
//    if (nosigdig + sigdig == 0)  /* no digits? */
//        return safe_number_zero();  /* invalid format */
//    *endptr = lua_cast(char *, s);  /* valid up to here */
//    e *= 4;  /* each digit multiplies/divides value by 2^4 */
//    if (*s == 'p' || *s == 'P') {  /* exponent part? */
//        int exp1 = 0;  /* exponent value */
//        int neg1;  /* exponent signal */
//        s++;  /* skip 'p' */
//        neg1 = isneg(&s);  /* signal */
//        if (!lisdigit(cast_uchar(*s)))
//            return safe_number_zero();  /* invalid; must have at least one digit */
//        while (lisdigit(cast_uchar(*s)))  /* read exponent */
//            exp1 = exp1 * 10 + *(s++) - '0';
//        if (neg1) exp1 = -exp1;
//        e += exp1;
//        *endptr = lua_cast(char *, s);  /* valid up to here */
//    }
//    if (neg) r = safe_number_neg(r);
//    return safe_number_create(std::to_string(l_mathop(ldexp)(std::stod(safe_number_to_string(r)), e)));
//}

#endif
/* }====================================================== */


static const char *l_str2d(const char *s, lua_Number *result) {
    char *endptr;
    if (strpbrk(s, "nN"))  /* reject 'inf' and 'nan' */
        return nullptr;
    //else if (strpbrk(s, "xX"))  /* hex? */
    //    *result = lua_strx2number(s, &endptr);
    else
        *result = safe_number_create(std::to_string(lua_str2number(s, &endptr)));
    if (endptr == s) return nullptr;  /* nothing recognized */
    while (lisspace(cast_uchar(*endptr))) endptr++;
    return (*endptr == '\0' ? endptr : nullptr);  /* OK if no trailing characters */
}


static const char *l_str2int(const char *s, lua_Integer *result) {
    lua_Unsigned a = 0;
    int empty = 1;
    int neg;
    while (lisspace(cast_uchar(*s))) s++;  /* skip initial spaces */
    neg = isneg(&s);
    if (s[0] == '0' &&
        (s[1] == 'x' || s[1] == 'X')) {  /* hex? */
        s += 2;  /* skip '0x' */
        for (; lisxdigit(cast_uchar(*s)); s++) {
            a = a * 16 + luaO_hexavalue(*s);
            empty = 0;
        }
    }
    else {  /* decimal */
        for (; lisdigit(cast_uchar(*s)); s++) {
            a = a * 10 + *s - '0';
            empty = 0;
        }
    }
    while (lisspace(cast_uchar(*s))) s++;  /* skip trailing spaces */
    if (empty || *s != '\0') return nullptr;  /* something wrong in the numeral */
    else {
        *result = l_castU2S((neg) ? 0u - a : a);
        return s;
    }
}


size_t luaO_str2num(const char *s, TValue *o) {
    lua_Integer i; lua_Number n;
    const char *e;
    if ((e = l_str2int(s, &i)) != nullptr) {  /* try as an integer */
        setivalue(o, i);
    }
    else if ((e = l_str2d(s, &n)) != nullptr) {  /* else try as a float */
        setfltvalue(o, n);
    }
    else
        return 0;  /* conversion failed */
    return (e - s) + 1;  /* success; return string size */
}


int luaO_utf8esc(char *buff, unsigned long x) {
    int n = 1;  /* number of bytes put in buffer (backwards) */
    lua_assert(x <= 0x10FFFF);
    if (x < 0x80)  /* ascii? */
        buff[UTF8BUFFSZ - 1] = lua_cast(char, x);
    else {  /* need continuation bytes */
        unsigned int mfb = 0x3f;  /* maximum that fits in first byte */
        do {  /* add continuation bytes */
            buff[UTF8BUFFSZ - (n++)] = lua_cast(char, 0x80 | (x & 0x3f));
            x >>= 6;  /* remove added bits */
            mfb >>= 1;  /* now there is one less bit available in first byte */
        } while (x > mfb);  /* still needs continuation byte? */
        buff[UTF8BUFFSZ - n] = lua_cast(char, (~mfb << 1) | x);  /* add first byte */
    }
    return n;
}


/* maximum length of the conversion of a number to a string */
#define MAXNUMBER2STR	50

size_t lua_number2str_impl(char* s, size_t sz, lua_Number n) {
	auto n_str = std::to_string(n);
	return l_sprintf(s, sz, "%s", n_str.c_str()); // LUA_NUMBER_FMT
}

/*
** Convert a number object to a string
*/
void luaO_tostring(lua_State *L, StkId obj) {
    char buff[MAXNUMBER2STR];
    size_t len;
    lua_assert(ttisnumber(obj));
    if (ttisinteger(obj))
        len = lua_integer2str(buff, sizeof(buff), ivalue(obj));
    else {
        len = lua_number2str(buff, sizeof(buff), fltvalue(obj));
        if (buff[strspn(buff, "-0123456789")] == '\0') {  /* looks like an int? */
            buff[len++] = lua_getlocaledecpoint();
            buff[len++] = '0';  /* adds '.0' to result */
        }
    }
    setsvalue2s(L, obj, luaS_newlstr(L, buff, len));
}


static void pushstr(lua_State *L, const char *str, size_t l) {
    setsvalue2s(L, L->top, luaS_newlstr(L, str, l));
    luaD_inctop(L);
}


/* this function handles only '%d', '%c', '%f', '%p', and '%s'
   conventional formats, plus Lua-specific '%I' and '%U' */
const char *luaO_pushvfstring(lua_State *L, const char *fmt, va_list argp) {
    int n = 0;
    for (;;) {
        const char *e = strchr(fmt, '%');
        if (e == nullptr) break;
        pushstr(L, fmt, e - fmt);
        switch (*(e + 1)) {
        case 's': {
            const char *s = va_arg(argp, char *);
            if (s == nullptr) s = "(null)";
            pushstr(L, s, strlen(s));
            break;
        }
        case 'c': {
            char buff = lua_cast(char, va_arg(argp, int));
            if (lisprint(cast_uchar(buff)))
                pushstr(L, &buff, 1);
            else  /* non-printable character; print its code */
                luaO_pushfstring(L, "<\\%d>", cast_uchar(buff));
            break;
        }
        case 'd': {
            setivalue(L->top, va_arg(argp, int));
            goto top2str;
        }
        case 'I': {
            setivalue(L->top, lua_cast(lua_Integer, va_arg(argp, l_uacInt)));
            goto top2str;
        }
        case 'f': {
            setfltvalue(L->top, cast_num(va_arg(argp, SafeNumber)));
        top2str:
            luaD_inctop(L);
            luaO_tostring(L, L->top - 1);
            break;
        }
        case 'p': {
            char buff[4 * sizeof(void *) + 8]; /* should be enough space for a '%p' */
            int l = l_sprintf(buff, sizeof(buff), "%p", va_arg(argp, void *));
            pushstr(L, buff, l);
            break;
        }
        case 'U': {
            char buff[UTF8BUFFSZ];
            int l = luaO_utf8esc(buff, lua_cast(long, va_arg(argp, long)));
            pushstr(L, buff + UTF8BUFFSZ - l, l);
            break;
        }
        case '%': {
            pushstr(L, "%", 1);
            break;
        }
        default: {
            luaG_runerror(L, "invalid option '%%%c' to 'lua_pushfstring'",
                *(e + 1));
        }
        }
        n += 2;
        fmt = e + 2;
    }
    luaD_checkstack(L, 1);
    pushstr(L, fmt, strlen(fmt));
    if (n > 0) luaV_concat(L, n + 1);
    return svalue(L->top - 1);
}


const char *luaO_pushfstring(lua_State *L, const char *fmt, ...) {
    const char *msg;
    va_list argp;
    va_start(argp, fmt);
    msg = luaO_pushvfstring(L, fmt, argp);
    va_end(argp);
    return msg;
}


/* number of chars of a literal string without the ending \0 */
#define LL(x)	(sizeof(x)/sizeof(char) - 1)

#define RETS	"..."
#define PRE	"[string \""
#define POS	"\"]"

#define addstr(a,b,l)	( memcpy(a,b,(l) * sizeof(char)), a += (l) )

void luaO_chunkid(char *out, const char *source, size_t bufflen) {
    size_t l = strlen(source);
    if (*source == '=') {  /* 'literal' source */
        if (l <= bufflen)  /* small enough? */
            memcpy(out, source + 1, l * sizeof(char));
        else {  /* truncate it */
            addstr(out, source + 1, bufflen - 1);
            *out = '\0';
        }
    }
    else if (*source == '@') {  /* file name */
        if (l <= bufflen)  /* small enough? */
            memcpy(out, source + 1, l * sizeof(char));
        else {  /* add '...' before rest of name */
            addstr(out, RETS, LL(RETS));
            bufflen -= LL(RETS);
            memcpy(out, source + 1 + l - bufflen, bufflen * sizeof(char));
        }
    }
    else {  /* string; format as [string "source"] */
        const char *nl = strchr(source, '\n');  /* find first new line (if any) */
        addstr(out, PRE, LL(PRE));  /* add prefix */
        bufflen -= LL(PRE RETS POS) + 1;  /* save space for prefix+suffix+'\0' */
        if (l < bufflen && nl == nullptr) {  /* small one-line source? */
            addstr(out, source, l);  /* keep it */
        }
        else {
            if (nl != nullptr) l = nl - source;  /* stop at first newline */
            if (l > bufflen) l = bufflen;
            addstr(out, source, l);
            addstr(out, RETS, LL(RETS));
        }
        memcpy(out, POS, (LL(POS) + 1) * sizeof(char));
    }
}

