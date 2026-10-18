/*
** Library for decimals. See ldecimal.h for the number format.
*/

#define ldeclib_c
#define LUA_LIB

#include "lprefix.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "ldecimal.h"


static void pushdec (lua_State *L, DecMant m, int e) {
  DecMag mag = m < 0 ? -(DecMag)m : (DecMag)m;
  lua_pushdecimal(L, m < 0, (lua_Unsigned)(mag >> 64), (lua_Unsigned)mag, e);
}


/* accepts a decimal or an integer */
static void checkdec (lua_State *L, int arg, DecMant *m, int *e) {
  int neg;
  lua_Unsigned hi, lo;
  if (lua_todecimal(L, arg, &neg, &hi, &lo, e)) {
    DecMant mag = ((DecMant)hi << 64) | lo;
    *m = neg ? -mag : mag;
  }
  else if (lua_isinteger(L, arg)) {
    *m = (DecMant)lua_tointeger(L, arg);
    *e = 0;
  }
  else
    luaL_typeerror(L, arg, "decimal");
}


typedef int (*BinOp) (DecMant, int, DecMant, int, DecMant *, int *);

static int binop (lua_State *L, BinOp op) {
  DecMant ma, mb, om;
  int ea, eb, oe;
  checkdec(L, 1, &ma, &ea);
  checkdec(L, 2, &mb, &eb);
  switch (op(ma, ea, mb, eb, &om, &oe)) {
    case DEC_OK: pushdec(L, om, oe); return 1;
    case DEC_DIVZERO: return luaL_error(L, "decimal division by zero");
    default: return luaL_error(L, "decimal overflow");
  }
}

static int dec_add (lua_State *L) { return binop(L, luaDec_add); }
static int dec_sub (lua_State *L) { return binop(L, luaDec_sub); }
static int dec_mul (lua_State *L) { return binop(L, luaDec_mul); }
static int dec_div (lua_State *L) { return binop(L, luaDec_div); }
static int dec_rem (lua_State *L) { return binop(L, luaDec_rem); }  /* truncated */


static int dec_neg (lua_State *L) {
  DecMant m; int e;
  checkdec(L, 1, &m, &e);
  pushdec(L, -m, e);
  return 1;
}

static int dec_abs (lua_State *L) {
  DecMant m; int e;
  checkdec(L, 1, &m, &e);
  pushdec(L, m < 0 ? -m : m, e);
  return 1;
}

static int dec_sign (lua_State *L) {
  DecMant m; int e;
  checkdec(L, 1, &m, &e);
  lua_pushinteger(L, (m > 0) - (m < 0));
  return 1;
}

static int dec_scale (lua_State *L) {
  DecMant m; int e;
  checkdec(L, 1, &m, &e);
  lua_pushinteger(L, -e);
  return 1;
}


static int round_to (lua_State *L, int places, DecRound mode) {
  DecMant m, om; int e, oe;
  checkdec(L, 1, &m, &e);
  if (luaDec_round(m, e, places, mode, &om, &oe) != DEC_OK)
    return luaL_error(L, "decimal overflow");
  pushdec(L, om, oe);
  return 1;
}

static int dec_round (lua_State *L) {
  static const char *const names[] = {"halfeven", "awayfromzero", "tozero",
                                      "tonegativeinfinity",
                                      "topositiveinfinity", NULL};
  lua_Integer places = luaL_checkinteger(L, 2);
  int mode = luaL_checkoption(L, 3, "halfeven", names);
  luaL_argcheck(L, 0 <= places && places <= DEC_MAXSCALE, 2,
                   "number of places out of range [0, 28]");
  return round_to(L, (int)places, (DecRound)mode);
}

static int dec_floor (lua_State *L) { return round_to(L, 0, DEC_TONEGINF); }
static int dec_ceil (lua_State *L) { return round_to(L, 0, DEC_TOPOSINF); }
static int dec_trunc (lua_State *L) { return round_to(L, 0, DEC_TOZERO); }


static int minmax (lua_State *L, int wantmax) {
  int n = lua_gettop(L), i;
  DecMant bm; int be;
  checkdec(L, 1, &bm, &be);
  for (i = 2; i <= n; i++) {
    DecMant m; int e, c;
    checkdec(L, i, &m, &e);
    c = luaDec_cmp(m, e, bm, be);
    /* for equal values max keeps the earlier argument, min the later */
    if (wantmax ? c > 0 : c <= 0) { bm = m; be = e; }
  }
  pushdec(L, bm, be);
  return 1;
}

static int dec_min (lua_State *L) { return minmax(L, 0); }
static int dec_max (lua_State *L) { return minmax(L, 1); }


static int dec_lt (lua_State *L) {
  DecMant ma, mb; int ea, eb;
  checkdec(L, 1, &ma, &ea);
  checkdec(L, 2, &mb, &eb);
  lua_pushboolean(L, luaDec_cmp(ma, ea, mb, eb) < 0);
  return 1;
}

static int dec_le (lua_State *L) {
  DecMant ma, mb; int ea, eb;
  checkdec(L, 1, &ma, &ea);
  checkdec(L, 2, &mb, &eb);
  lua_pushboolean(L, luaDec_cmp(ma, ea, mb, eb) <= 0);
  return 1;
}


static int dec_tostring (lua_State *L) {
  DecMant m; int e;
  char buff[DEC_MAXSTR];
  size_t len;
  checkdec(L, 1, &m, &e);
  len = luaDec_tostr(buff, m, e);
  lua_pushlstring(L, buff, len);
  return 1;
}

static int dec_tonumber (lua_State *L) {
  DecMant m; int e;
  char buff[DEC_MAXSTR];
  checkdec(L, 1, &m, &e);
  luaDec_tostr(buff, m, e);
  lua_pushnumber(L, strtod(buff, NULL));  /* may lose precision */
  return 1;
}


static int pushparsed (lua_State *L, const char *s, size_t n) {
  DecMant m; int e;
  if (luaDec_parse(s, n, &m, &e, NULL))
    pushdec(L, m, e);
  else
    luaL_pushfail(L);
  return 1;
}

static int dec_fromstring (lua_State *L) {
  size_t n;
  const char *s = luaL_checklstring(L, 1, &n);
  return pushparsed(L, s, n);
}

/*
** decimal.create(x): converts an integer, a float, a string or a decimal.
** A float converts through its shortest round-trip digits, so
** decimal.create(0.1) is 0.1 exactly. Returns fail if it does not fit.
*/
static int dec_create (lua_State *L) {
  switch (lua_type(L, 1)) {
    case LUA_TDECIMAL:
      lua_settop(L, 1);
      return 1;
    case LUA_TNUMBER: {
      if (lua_isinteger(L, 1)) {
        pushdec(L, (DecMant)lua_tointeger(L, 1), 0);
        return 1;
      }
      else {
        lua_Number v = lua_tonumber(L, 1);
        char buff[LUA_N2SBUFFSZ];
        unsigned len;
        if (!isfinite(v)) { luaL_pushfail(L); return 1; }
        len = lua_numbertocstring(L, 1, buff);  /* shortest round-trip */
        return pushparsed(L, buff, len - 1);
      }
    }
    case LUA_TSTRING: {
      size_t n;
      const char *s = lua_tolstring(L, 1, &n);
      return pushparsed(L, s, n);
    }
    default:
      return luaL_typeerror(L, 1, "number, string or decimal");
  }
}


static const luaL_Reg declib[] = {
  {"create", dec_create},
  {"fromstring", dec_fromstring},
  {"tostring", dec_tostring},
  {"tonumber", dec_tonumber},
  {"neg", dec_neg},
  {"abs", dec_abs},
  {"sign", dec_sign},
  {"scale", dec_scale},
  {"add", dec_add},
  {"sub", dec_sub},
  {"mul", dec_mul},
  {"div", dec_div},
  {"rem", dec_rem},
  {"round", dec_round},
  {"floor", dec_floor},
  {"ceil", dec_ceil},
  {"trunc", dec_trunc},
  {"min", dec_min},
  {"max", dec_max},
  {"lt", dec_lt},
  {"le", dec_le},
  {NULL, NULL}
};


LUAMOD_API int luaopen_decimal (lua_State *L) {
  luaL_newlib(L, declib);
  return 1;
}
