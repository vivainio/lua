/*
** Decimal arithmetic.
**
** Arithmetic is computed exactly in 256 bits and then rounded half-even,
** once, to the 90 bits that fit in the mantissa; that lowers the scale.
** A value that cannot be represented even at scale 0 is an overflow.
**
** The 256-bit helper uses eight 32-bit limbs so that every intermediate
** product fits in 64 bits. (Only DecMant itself needs __int128.)
*/

#define ldecimal_c
#define LUA_CORE

#include "lprefix.h"

#include <string.h>

#include "lua.h"
#include "ldecimal.h"


/* {====================================================================
** Unsigned 256-bit integers
** ===================================================================== */

#define WLIMBS		8
#define WMAXPOW10	77  /* 10^77 < 2^256 < 10^78 */

typedef struct Wide { uint32_t w[WLIMBS]; } Wide;

static Wide wzero (void) {
  Wide r;
  memset(&r, 0, sizeof(r));
  return r;
}

static Wide wfromu64 (uint64_t v) {
  Wide r = wzero();
  r.w[0] = (uint32_t)v;
  r.w[1] = (uint32_t)(v >> 32);
  return r;
}

static int wiszero (Wide a) {
  uint32_t x = 0;
  int i;
  for (i = 0; i < WLIMBS; i++) x |= a.w[i];
  return x == 0;
}

static int wcmp (Wide a, Wide b) {
  int i;
  for (i = WLIMBS - 1; i >= 0; i--) {
    if (a.w[i] != b.w[i]) return a.w[i] < b.w[i] ? -1 : 1;
  }
  return 0;
}

static Wide wadd (Wide a, Wide b) {
  Wide r;
  uint64_t carry = 0;
  int i;
  for (i = 0; i < WLIMBS; i++) {
    carry += (uint64_t)a.w[i] + b.w[i];
    r.w[i] = (uint32_t)carry;
    carry >>= 32;
  }
  return r;
}

/* requires a >= b */
static Wide wsub (Wide a, Wide b) {
  Wide r;
  uint64_t borrow = 0;
  int i;
  for (i = 0; i < WLIMBS; i++) {
    uint64_t t = (uint64_t)a.w[i] - b.w[i] - borrow;
    r.w[i] = (uint32_t)t;
    borrow = (t >> 63) & 1;
  }
  return r;
}

/* the low 256 bits of a * b */
static Wide wmul (Wide a, Wide b) {
  Wide r = wzero();
  int i, j;
  for (i = 0; i < WLIMBS; i++) {
    uint64_t carry = 0;
    if (a.w[i] == 0) continue;
    for (j = 0; i + j < WLIMBS; j++) {
      uint64_t t = (uint64_t)a.w[i] * b.w[j] + r.w[i + j] + carry;
      r.w[i + j] = (uint32_t)t;
      carry = t >> 32;
    }
  }
  return r;
}

static Wide wmulsmall (Wide a, uint32_t m) {
  Wide r;
  uint64_t carry = 0;
  int i;
  for (i = 0; i < WLIMBS; i++) {
    uint64_t t = (uint64_t)a.w[i] * m + carry;
    r.w[i] = (uint32_t)t;
    carry = t >> 32;
  }
  return r;
}

/* divides by a nonzero 32-bit divisor, returns the remainder */
static Wide wdivsmall (Wide a, uint32_t d, uint32_t *rem) {
  Wide q = wzero();
  uint64_t r = 0;
  int i;
  for (i = WLIMBS - 1; i >= 0; i--) {
    uint64_t cur = (r << 32) | a.w[i];
    q.w[i] = (uint32_t)(cur / d);
    r = cur % d;
  }
  *rem = (uint32_t)r;
  return q;
}

static int wclz32 (uint32_t x) {
  int n = 0;
  while ((x & 0x80000000u) == 0) { x <<= 1; n++; }
  return n;
}

static int wbitlen (Wide a) {
  int i;
  for (i = WLIMBS - 1; i >= 0; i--) {
    if (a.w[i] != 0) return i * 32 + 32 - wclz32(a.w[i]);
  }
  return 0;
}

/* n = q * d + r with r < d; d must not be zero. Knuth's algorithm D
   (TAOCP 4.3.1) on 32-bit limbs, in the form given in Hacker's Delight. */
static void wdivmod (Wide n, Wide d, Wide *q, Wide *r) {
  int nn = WLIMBS, dn = WLIMBS, s, i, j;
  uint32_t vn[WLIMBS], un[WLIMBS + 1];
  const uint64_t base = (uint64_t)1 << 32;
  while (nn > 0 && n.w[nn - 1] == 0) nn--;
  while (dn > 0 && d.w[dn - 1] == 0) dn--;
  *q = wzero();
  *r = wzero();
  if (nn < dn) { *r = n; return; }
  if (dn == 1) {
    uint32_t rem;
    *q = wdivsmall(n, d.w[0], &rem);
    r->w[0] = rem;
    return;
  }
  s = wclz32(d.w[dn - 1]);  /* normalise so the divisor's top bit is set */
  for (i = dn - 1; i > 0; i--)
    vn[i] = (d.w[i] << s) | (s ? d.w[i - 1] >> (32 - s) : 0);
  vn[0] = d.w[0] << s;
  un[nn] = s ? n.w[nn - 1] >> (32 - s) : 0;
  for (i = nn - 1; i > 0; i--)
    un[i] = (n.w[i] << s) | (s ? n.w[i - 1] >> (32 - s) : 0);
  un[0] = n.w[0] << s;
  for (j = nn - dn; j >= 0; j--) {
    int64_t borrow = 0, t;
    uint64_t num = ((uint64_t)un[j + dn] << 32) | un[j + dn - 1];
    uint64_t qhat = num / vn[dn - 1];
    uint64_t rhat = num - qhat * vn[dn - 1];
    while (qhat >= base || qhat * vn[dn - 2] > ((rhat << 32) | un[j + dn - 2])) {
      qhat--;
      rhat += vn[dn - 1];
      if (rhat >= base) break;
    }
    for (i = 0; i < dn; i++) {  /* multiply and subtract */
      uint64_t p = qhat * vn[i];
      t = (int64_t)un[i + j] - borrow - (int64_t)(p & 0xFFFFFFFFu);
      un[i + j] = (uint32_t)t;
      borrow = (int64_t)(p >> 32) - (t >> 32);
    }
    t = (int64_t)un[j + dn] - borrow;
    un[j + dn] = (uint32_t)t;
    q->w[j] = (uint32_t)qhat;
    if (t < 0) {  /* the estimate was one too large (rare): add back */
      uint64_t carry = 0;
      q->w[j]--;
      for (i = 0; i < dn; i++) {
        carry += (uint64_t)un[i + j] + vn[i];
        un[i + j] = (uint32_t)carry;
        carry >>= 32;
      }
      un[j + dn] += (uint32_t)carry;
    }
  }
  for (i = 0; i < dn; i++)
    r->w[i] = (un[i] >> s) | (s ? un[i + 1] << (32 - s) : 0);
}

static Wide pow10tab[WMAXPOW10 + 1];
static int pow10ready = 0;

static Wide wpow10 (int n) {
  if (!pow10ready) {  /* idempotent, so a race only repeats the work */
    int i;
    pow10tab[0] = wfromu64(1);
    for (i = 1; i <= WMAXPOW10; i++) pow10tab[i] = wmulsmall(pow10tab[i - 1], 10);
    pow10ready = 1;
  }
  return pow10tab[n];
}

/* number of decimal digits, 0 for zero */
static int wdigits (Wide a) {
  int bits = wbitlen(a), n;
  if (bits == 0) return 0;
  n = (bits * 1233) >> 12;  /* floor(bits * log10(2)) is within one */
  if (n > WMAXPOW10 + 1) n = WMAXPOW10 + 1;
  while (n <= WMAXPOW10 && wcmp(a, wpow10(n)) >= 0) n++;
  while (n > 0 && wcmp(a, wpow10(n - 1)) < 0) n--;
  return n;
}

/* }==================================================================== */


typedef struct SignMag { Wide mag; int neg; } SignMag;

static Wide maxmant (void) {  /* 2^90 - 1 */
  Wide w = wzero();
  w.w[0] = 0xFFFFFFFFu;
  w.w[1] = 0xFFFFFFFFu;
  w.w[2] = 0x03FFFFFFu;
  return w;
}

static SignMag tosm (DecMant m) {
  SignMag r;
  DecMag mag = m < 0 ? -(DecMag)m : (DecMag)m;
  r.mag = wfromu64((uint64_t)mag);
  r.mag.w[2] = (uint32_t)(mag >> 64);
  r.mag.w[3] = (uint32_t)(mag >> 96);
  r.neg = m < 0;
  return r;
}

/* the magnitude must not exceed 2^90 - 1 */
static DecMant frommag (Wide mag, int neg) {
  DecMag v = ((DecMag)mag.w[2] << 64) | ((DecMag)mag.w[1] << 32) | mag.w[0];
  return neg ? -(DecMant)v : (DecMant)v;
}

/* divides a by 10^drop, rounding half-even */
static Wide dropdigits (Wide a, int drop) {
  Wide div, q, r;
  int c;
  if (drop <= 0) return a;
  if (drop > WMAXPOW10) return wzero();  /* a < 2^256 < 10^78 */
  div = wpow10(drop);
  wdivmod(a, div, &q, &r);
  c = wcmp(wadd(r, r), div);
  if (c > 0 || (c == 0 && (q.w[0] & 1))) q = wadd(q, wfromu64(1));
  return q;
}

/*
** Reduces mag * 10^e to a magnitude that fits the mantissa and an exponent
** in [-28, 0], rounding half-even once, to the finest scale that makes it
** fit. Fails if the value does not fit even at scale 0: an integer part
** that does not fit is an overflow, it is never rounded.
*/
static int reduce (Wide *mag, int *e) {
  Wide maxm = maxmant();
  int target, fit;
  if (*e <= 0 && *e >= -DEC_MAXSCALE && wcmp(*mag, maxm) <= 0) return 1;
  if (wiszero(*mag)) {
    *e = *e > 0 ? 0 : (*e < -DEC_MAXSCALE ? -DEC_MAXSCALE : *e);
    return 1;
  }
  if (*e > 0) {  /* an integer in scientific notation */
    if (*e > WMAXPOW10 || wdigits(*mag) + *e > 28) return 0;
    *mag = wmul(*mag, wpow10(*e));
    *e = 0;
    return wcmp(*mag, maxm) <= 0;
  }
  target = *e < -DEC_MAXSCALE ? -DEC_MAXSCALE : *e;
  fit = *e + (wdigits(*mag) - 28);
  if (fit > target) target = fit;
  for (;;) {
    Wide q;
    if (target > 0) return 0;
    q = dropdigits(*mag, target - *e);
    if (wcmp(q, maxm) <= 0) { *mag = q; *e = target; return 1; }
    target++;  /* rounding carried into a 29th digit, or estimate short */
  }
}

/* removes trailing zeros while the scale is above the preferred one */
static void stripzeros (Wide *mag, int *e, int preferred) {
  static const uint32_t p10[10] = {1, 10, 100, 1000, 10000, 100000,
                                   1000000, 10000000, 100000000, 1000000000};
  static const int chunks[4] = {9, 4, 2, 1};
  int allow = -*e - preferred, c;
  if (allow <= 0 || wiszero(*mag)) return;
  for (c = 0; c < 4; c++) {
    int chunk = chunks[c];
    while (allow >= chunk) {
      uint32_t rem;
      Wide q = wdivsmall(*mag, p10[chunk], &rem);
      if (rem != 0) break;
      *mag = q;
      *e += chunk;
      allow -= chunk;
    }
  }
}

static int finish (Wide mag, int neg, int e, DecMant *om, int *oe) {
  if (!reduce(&mag, &e)) return DEC_OVERFLOW;
  *om = frommag(mag, neg && !wiszero(mag));
  *oe = e;
  return DEC_OK;
}

static int addsub (DecMant ma, int ea, DecMant mb, int eb, int subtract,
                   DecMant *om, int *oe) {
  SignMag a = tosm(ma), b = tosm(mb);
  Wide mag;
  int neg, c;
  int e = ea < eb ? ea : eb;
  if (subtract) b.neg = !b.neg;
  a.mag = wmul(a.mag, wpow10(ea - e));  /* factor is at most 10^28 */
  b.mag = wmul(b.mag, wpow10(eb - e));
  if (a.neg == b.neg) { mag = wadd(a.mag, b.mag); neg = a.neg; }
  else if ((c = wcmp(a.mag, b.mag)) == 0) { mag = wzero(); neg = 0; }
  else if (c > 0) { mag = wsub(a.mag, b.mag); neg = a.neg; }
  else { mag = wsub(b.mag, a.mag); neg = b.neg; }
  return finish(mag, neg, e, om, oe);
}

int luaDec_add (DecMant ma, int ea, DecMant mb, int eb, DecMant *om, int *oe) {
  DecMant r = ma + mb;  /* magnitudes below 2^90: cannot overflow 128 bits */
  if (ea == eb && r <= DEC_MAXMANT && r >= -DEC_MAXMANT) {
    *om = r; *oe = ea;  /* fast path */
    return DEC_OK;
  }
  return addsub(ma, ea, mb, eb, 0, om, oe);
}

int luaDec_sub (DecMant ma, int ea, DecMant mb, int eb, DecMant *om, int *oe) {
  DecMant r = ma - mb;
  if (ea == eb && r <= DEC_MAXMANT && r >= -DEC_MAXMANT) {
    *om = r; *oe = ea;
    return DEC_OK;
  }
  return addsub(ma, ea, mb, eb, 1, om, oe);
}

int luaDec_mul (DecMant ma, int ea, DecMant mb, int eb, DecMant *om, int *oe) {
  SignMag a, b;
  DecMant r;
  if (ea + eb >= -DEC_MAXSCALE && !__builtin_mul_overflow(ma, mb, &r) &&
      r <= DEC_MAXMANT && r >= -DEC_MAXMANT) {
    *om = r; *oe = ea + eb;
    return DEC_OK;
  }
  a = tosm(ma); b = tosm(mb);
  return finish(wmul(a.mag, b.mag), a.neg != b.neg, ea + eb, om, oe);
}

/*
** The quotient has as many digits as fit, then trailing zeros are removed
** down to the preferred scale max(scale(a) - scale(b), 0).
*/
int luaDec_div (DecMant ma, int ea, DecMant mb, int eb, DecMant *om, int *oe) {
  SignMag a = tosm(ma), b = tosm(mb);
  Wide maxm = maxmant(), maxdiv10, q;
  uint32_t rem;
  int s0 = eb - ea;  /* scale(a) - scale(b) */
  int preferred = s0 > 0 ? s0 : 0;
  int na, nb, kmin, kmax, k, e;
  if (wiszero(b.mag)) return DEC_DIVZERO;
  if (wiszero(a.mag)) { *om = 0; *oe = -preferred; return DEC_OK; }
  maxdiv10 = wdivsmall(maxm, 10, &rem);
  na = wdigits(a.mag);
  nb = wdigits(b.mag);
  /* the quotient a / b * 10^k has scale s0 + k, which must be in [0, 28],
     and the scaled numerator must fit in 256 bits */
  kmin = s0 < 0 ? -s0 : 0;
  kmax = DEC_MAXSCALE - s0;
  if (kmax > WMAXPOW10 - na) kmax = WMAXPOW10 - na;
  if (kmin > kmax) return DEC_OVERFLOW;
  k = 27 - (na - nb);  /* aim for 27 digits */
  k = k < kmin ? kmin : (k > kmax ? kmax : k);
#define QUOTIENT(kk, out, fits) { \
    Wide qq, rr; int cc; \
    wdivmod(wmul(a.mag, wpow10(kk)), b.mag, &qq, &rr); \
    cc = wcmp(wadd(rr, rr), b.mag); \
    if (cc > 0 || (cc == 0 && (qq.w[0] & 1))) qq = wadd(qq, wfromu64(1)); \
    *(out) = qq; fits = wcmp(qq, maxm) <= 0; }
  for (;;) {
    int fits;
    QUOTIENT(k, &q, fits);
    if (fits) break;
    if (k == kmin) return DEC_OVERFLOW;
    k--;
  }
  /* the quotient grows about tenfold per scale, so only try a finer scale
     if it is small enough */
  while (k < kmax && wcmp(q, maxdiv10) <= 0) {
    Wide next; int fits;
    QUOTIENT(k + 1, &next, fits);
    if (!fits) break;
    k++;
    q = next;
  }
#undef QUOTIENT
  if (wiszero(q)) { *om = 0; *oe = 0; return DEC_OK; }  /* as in .NET */
  e = -(s0 + k);
  stripzeros(&q, &e, preferred);
  *om = frommag(q, (a.neg != b.neg) && !wiszero(q));
  *oe = e;
  return DEC_OK;
}

/* remainder with the sign of the dividend; scale is the larger of the two */
int luaDec_rem (DecMant ma, int ea, DecMant mb, int eb, DecMant *om, int *oe) {
  SignMag a = tosm(ma), b = tosm(mb);
  Wide x, y, q, r;
  int e = ea < eb ? ea : eb;
  if (wiszero(b.mag)) return DEC_DIVZERO;
  x = wmul(a.mag, wpow10(ea - e));
  y = wmul(b.mag, wpow10(eb - e));
  wdivmod(x, y, &q, &r);
  return finish(r, a.neg, e, om, oe);
}

int luaDec_cmp (DecMant ma, int ea, DecMant mb, int eb) {
  int sa = (ma > 0) - (ma < 0), sb = (mb > 0) - (mb < 0), r, e;
  SignMag a, b;
  if (sa != sb) return sa < sb ? -1 : 1;
  if (sa == 0) return 0;
  if (ea == eb) {
    r = (ma > mb) - (ma < mb);
    return r;
  }
  a = tosm(ma); b = tosm(mb);
  e = ea < eb ? ea : eb;
  r = wcmp(wmul(a.mag, wpow10(ea - e)), wmul(b.mag, wpow10(eb - e)));
  return sa < 0 ? -r : r;
}

/* rounds to 'places' digits after the point; finer values are unchanged */
int luaDec_round (DecMant m, int e, int places, DecRound mode,
                  DecMant *om, int *oe) {
  int target = -places, half, up = 0;
  SignMag a;
  Wide div, q, rem;
  if (e >= target) { *om = m; *oe = e; return DEC_OK; }
  a = tosm(m);
  div = wpow10(target - e);
  wdivmod(a.mag, div, &q, &rem);
  half = wcmp(wadd(rem, rem), div);
  switch (mode) {
    case DEC_HALFEVEN: up = half > 0 || (half == 0 && (q.w[0] & 1)); break;
    case DEC_AWAYFROMZERO: up = half >= 0; break;
    case DEC_TOZERO: break;
    case DEC_TONEGINF: up = a.neg && !wiszero(rem); break;
    case DEC_TOPOSINF: up = !a.neg && !wiszero(rem); break;
  }
  if (up) q = wadd(q, wfromu64(1));
  return finish(q, a.neg, target, om, oe);
}

int luaDec_parse (const char *s, size_t n, DecMant *om, int *oe,
                  int *exact) {
  Wide mag = wzero(), cap = wpow10(70);
  int neg = 0, e = 0, any = 0, dot = 0, lost = 0, e0;
  size_t i = 0;
  while (n > 0 && (s[n - 1] == ' ' || (s[n - 1] >= '\t' && s[n - 1] <= '\r')))
    n--;
  while (i < n && (s[i] == ' ' || (s[i] >= '\t' && s[i] <= '\r'))) i++;
  if (i < n && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
  for (; i < n; i++) {
    char c = s[i];
    if (c >= '0' && c <= '9') {
      any = 1;
      if (wcmp(mag, cap) < 0) {  /* more digits are far below precision */
        mag = wadd(wmulsmall(mag, 10), wfromu64((uint64_t)(c - '0')));
        if (dot) e--;
      }
      else {
        lost = lost || c != '0';
        if (!dot) e++;
      }
    }
    else if (c == '.' && !dot) dot = 1;
    else break;
  }
  if (!any) return 0;
  if (i < n && (s[i] == 'e' || s[i] == 'E')) {
    int eneg = 0;
    long ex = 0;
    i++;
    if (i < n && (s[i] == '+' || s[i] == '-')) eneg = s[i++] == '-';
    if (i >= n) return 0;
    for (; i < n && s[i] >= '0' && s[i] <= '9'; i++)
      if (ex < 100000) ex = ex * 10 + (s[i] - '0');
    e += (int)(eneg ? -ex : ex);
  }
  if (i != n) return 0;
  e0 = e;
  if (finish(mag, neg, e, om, oe) != DEC_OK) return 0;
  if (exact) {  /* exact if the result, brought back to the old exponent... */
    Wide rm = tosm(*om).mag, back;
    int d = *oe - e0;
    if (wiszero(mag)) back = rm;
    else if (d >= 0 && d <= WMAXPOW10) back = wmul(rm, wpow10(d));
    else if (d < 0 && -d <= WMAXPOW10) back = rm, mag = wmul(mag, wpow10(-d));
    else back = wmulsmall(rm, 2);  /* out of range: not equal */
    *exact = !lost && wcmp(back, mag) == 0;
  }
  return 1;
}

size_t luaDec_tostr (char *buf, DecMant m, int e) {
  char digits[48], tmp[48];
  DecMag mag = m < 0 ? -(DecMag)m : (DecMag)m;
  int nd = 0, n, scale = -e;
  char *out = buf;
  do { tmp[nd++] = (char)('0' + (int)(mag % 10)); mag /= 10; } while (mag != 0);
  for (n = 0; n < nd; n++) digits[n] = tmp[nd - 1 - n];
  if (m < 0) *out++ = '-';
  if (nd > scale) {  /* has an integer part */
    memcpy(out, digits, nd - scale); out += nd - scale;
    if (scale > 0) {
      *out++ = '.';
      memcpy(out, digits + nd - scale, scale); out += scale;
    }
  }
  else {
    *out++ = '0'; *out++ = '.';
    memset(out, '0', scale - nd); out += scale - nd;
    memcpy(out, digits, nd); out += nd;
  }
  *out = '\0';
  return (size_t)(out - buf);
}


/*
** {====================================================================
** The Decimal object
** =====================================================================
*/

#include "ldebug.h"
#include "lgc.h"
#include "lstate.h"

Decimal *luaDec_new (lua_State *L, DecMant m, int e) {
  GCObject *o = luaC_newobj(L, LUA_VDECIMAL, sizeof(Decimal));
  Decimal *d = gco2dec(o);
  DecMag mag = m < 0 ? -(DecMag)m : (DecMag)m;
  lua_assert(e <= 0 && e >= -DEC_MAXSCALE && mag <= (DecMag)DEC_MAXMANT);
  d->neg = (m < 0);
  d->scale = cast_byte(-e);
  d->hi = (unsigned int)(mag >> 64);
  d->lo = (lua_Unsigned)mag;
  return d;
}

DecMant luaDec_mant (const Decimal *d) {
  DecMant mag = ((DecMant)d->hi << 64) | d->lo;
  return d->neg ? -mag : mag;
}

int luaDec_equal (const Decimal *a, const Decimal *b) {
  if (a == b) return 1;
  if (a->scale == b->scale)  /* same scale: the bits are the value */
    return a->lo == b->lo && a->hi == b->hi && a->neg == b->neg;
  return luaDec_cmp(luaDec_mant(a), luaDec_exp(a),
                    luaDec_mant(b), luaDec_exp(b)) == 0;
}

int luaDec_get (const TValue *o, DecMant *m, int *e) {
  if (ttisdecimal(o)) {
    *m = luaDec_mant(decvalue(o));
    *e = luaDec_exp(decvalue(o));
    return 1;
  }
  else if (ttisinteger(o)) {  /* integers mix in implicitly */
    *m = (DecMant)ivalue(o);
    *e = 0;
    return 1;
  }
  return 0;
}

static l_noret mixerror (lua_State *L, const char *what) {
  luaG_runerror(L, "attempt to %s a decimal and a float "
                   "(use decimal.create to convert)", what);
}

int luaDec_arith (lua_State *L, const TValue *p1, const TValue *p2,
                  StkId res, TMS event) {
  DecMant ma, mb, om;
  int ea, eb, oe, st;
  if (!ttisdecimal(p1) && !ttisdecimal(p2)) return 0;
  if (!luaDec_get(p1, &ma, &ea) || !luaDec_get(p2, &mb, &eb)) {
    if (ttisnumber(p1) || ttisnumber(p2))  /* a float */
      mixerror(L, "perform arithmetic on");
    return 0;  /* other types: generic error */
  }
  switch (event) {
    case TM_ADD: st = luaDec_add(ma, ea, mb, eb, &om, &oe); break;
    case TM_SUB: st = luaDec_sub(ma, ea, mb, eb, &om, &oe); break;
    case TM_MUL: st = luaDec_mul(ma, ea, mb, eb, &om, &oe); break;
    case TM_DIV: st = luaDec_div(ma, ea, mb, eb, &om, &oe); break;
    case TM_MOD:  /* floored, like '%' on numbers */
      st = luaDec_rem(ma, ea, mb, eb, &om, &oe);
      if (st == DEC_OK && om != 0 && ((om < 0) != (mb < 0)))
        st = luaDec_add(om, oe, mb, eb, &om, &oe);
      break;
    case TM_UNM: om = -ma; oe = ea; st = DEC_OK; break;
    default: return 0;  /* '//', '^' and bitwise operations are errors */
  }
  if (st == DEC_DIVZERO) luaG_runerror(L, "attempt to divide a decimal by zero");
  if (st != DEC_OK) luaG_runerror(L, "decimal overflow");
  setdecvalue(L, s2v(res), luaDec_new(L, om, oe));
  return 1;
}

int luaDec_isorder (const TValue *p1, const TValue *p2) {
  return ttisdecimal(p1) || ttisdecimal(p2);
}

int luaDec_order (lua_State *L, const TValue *p1, const TValue *p2,
                  TMS event) {
  DecMant ma, mb;
  int ea, eb, c;
  if (!luaDec_get(p1, &ma, &ea) || !luaDec_get(p2, &mb, &eb)) {
    if (ttisnumber(p1) || ttisnumber(p2))
      mixerror(L, "compare");
    luaG_ordererror(L, p1, p2);
  }
  c = luaDec_cmp(ma, ea, mb, eb);
  return event == TM_LT ? c < 0 : c <= 0;
}

/* }==================================================================== */
