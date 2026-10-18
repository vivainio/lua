/*
** Decimal numbers: exact base-10 arithmetic with the semantics of .NET's
** System.Decimal. See ldecimal.c.
*/

#ifndef ldecimal_h
#define ldecimal_h

#include <stddef.h>
#include <stdint.h>

#include "lua.h"
#include "lobject.h"
#include "lstate.h"

#if !defined(__SIZEOF_INT128__)
#error "decimal values currently require __int128"
#endif

/*
** A decimal is 'mantissa * 10^exponent'. The mantissa is a signed integer
** with a magnitude below 2^90 (27 digits always fit, 28 up to 1.24e27) and
** the exponent is the negated scale, always in [-28, 0]. The scale is
** preserved (2.50 is 250e-2) and comparison ignores it.
*/
typedef __int128 DecMant;
typedef unsigned __int128 DecMag;

#define DEC_MAXSCALE	28
#define DEC_MAXMANT	((((DecMant)1) << 90) - 1)

#define DEC_OK		0
#define DEC_OVERFLOW	1
#define DEC_DIVZERO	2

#define DEC_MAXSTR	64

LUAI_FUNC int luaDec_add (DecMant ma, int ea, DecMant mb, int eb,
                          DecMant *om, int *oe);
LUAI_FUNC int luaDec_sub (DecMant ma, int ea, DecMant mb, int eb,
                          DecMant *om, int *oe);
LUAI_FUNC int luaDec_mul (DecMant ma, int ea, DecMant mb, int eb,
                          DecMant *om, int *oe);
LUAI_FUNC int luaDec_div (DecMant ma, int ea, DecMant mb, int eb,
                          DecMant *om, int *oe);
LUAI_FUNC int luaDec_rem (DecMant ma, int ea, DecMant mb, int eb,
                          DecMant *om, int *oe);
LUAI_FUNC int luaDec_cmp (DecMant ma, int ea, DecMant mb, int eb);

typedef enum {
  DEC_HALFEVEN, DEC_AWAYFROMZERO, DEC_TOZERO, DEC_TONEGINF, DEC_TOPOSINF
} DecRound;

LUAI_FUNC int luaDec_round (DecMant m, int e, int places, DecRound mode,
                            DecMant *om, int *oe);

/* parses [+-]digits[.digits][(e|E)[+-]digits]; returns 0 if malformed or
   if the value does not fit. Excess digits are rounded half-even, and
   '*exact' (if not NULL) tells whether any rounding happened. */
LUAI_FUNC int luaDec_parse (const char *s, size_t n, DecMant *om, int *oe,
                            int *exact);

/* writes plain notation (no exponent), returns the length */
LUAI_FUNC size_t luaDec_tostr (char *buf, DecMant m, int e);

/* {==== the Decimal object and the VM's use of it ==== */

LUAI_FUNC Decimal *luaDec_new (lua_State *L, DecMant m, int e);
LUAI_FUNC DecMant luaDec_mant (const Decimal *d);
#define luaDec_exp(d)	(-cast_int((d)->scale))
LUAI_FUNC int luaDec_equal (const Decimal *a, const Decimal *b);

/* arithmetic metamethod fallback for 'event' (an arithmetic TMS); returns
   0 if no decimal is involved, raises an error for mixed or invalid use */
LUAI_FUNC int luaDec_arith (lua_State *L, const TValue *p1, const TValue *p2,
                            StkId res, TMS event);
LUAI_FUNC int luaDec_isorder (const TValue *p1, const TValue *p2);
LUAI_FUNC int luaDec_order (lua_State *L, const TValue *p1, const TValue *p2,
                            TMS event);

/* a decimal or an integer as a mantissa and an exponent */
LUAI_FUNC int luaDec_get (const TValue *o, DecMant *m, int *e);


#endif
