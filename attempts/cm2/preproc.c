/*
 * preproc.c - step 4: a reversible text transform in front of the model.
 * Included by cm2.c, so cm2 stays one self-contained program.
 *
 * The compressor rewrites the input into a stream T that the model codes;
 * the decompressor decodes T and runs the inverse. Three parts, each optional:
 *
 *   capitals    "The" -> CAP "the", "THE" -> ALLCAP "the", so all forms of a
 *               word share statistics. Mixed-case words ("McDonald") are
 *               left as they are.
 *   entities    &amp; &quot; &lt; &gt; -> one byte each.
 *   dictionary  frequent words -> a 1- or 2-byte code. The dictionary is
 *               chosen from the input itself and sent at the start of T,
 *               where the model compresses it like any other text.
 *
 * Flags, entity bytes and codes use only byte values that never occur in the
 * input, so no escaping is ever needed. Which byte values are free is sent in
 * a small raw header; both sides derive the same roles from it. When too few
 * byte values are free (binary data), the transform switches itself off.
 *
 * The compressor runs the inverse over T before using it and compares the
 * result with the input, so a transform bug falls back to "off" instead of
 * producing a broken archive.
 */

#ifndef PP_CAPS
#define PP_CAPS 1        /* capital flags */
#endif
#ifndef PP_ENT
#define PP_ENT 1         /* entity bytes */
#endif
#ifndef PP_DICT
#define PP_DICT 1        /* word dictionary */
#endif
#ifndef PP_K1
#define PP_K1 24         /* how many words get a one-byte code (step 7 on enwik8: 24 beat 20, 32, 40) */
#endif
#ifndef PP_MINLEN
#define PP_MINLEN 2      /* shortest word worth a code */
#endif
#ifndef PP_MINCOUNT
#define PP_MINCOUNT 8    /* fewest occurrences worth a code */
#endif
#ifndef PP_MAXDICT
#define PP_MAXDICT 1000000 /* cap on dictionary size (also capped by free bytes) */
#endif
#ifndef PP_SORT
#define PP_SORT 1        /* order of 2-byte codes: 0 frequency, 1 alphabetical, 2 by suffix */
#endif
#ifndef PP_HBITS
#define PP_HBITS 23      /* word counting table: 2^23 entries x 16 bytes = 128 MB */
#endif

/* Second byte of a two-byte code: 0x80..0xff. These never look like ASCII
   punctuation, so the step 3 parse state is not disturbed by them. */
#define PP_B2 128

enum { R_NONE, R_CAP, R_ALLCAP, R_ENT, R_CODE1, R_PREFIX };
static U8 pp_role[256];  /* role of each byte value in T (all R_NONE when off) */
static U8 pp_arg[256];   /* entity index, one-byte code index or prefix index */

#define PP_NENT 4
static const char *const pp_ent[PP_NENT] = {"&amp;", "&quot;", "&lt;", "&gt;"};
static const int pp_entlen[PP_NENT] = {5, 6, 4, 4};

#define PP_FEAT_CAPS 1
#define PP_FEAT_ENT 2
#define PP_FEAT_DICT 4

typedef struct {
  U8 feat;          /* 0 = transform off */
  U8 k1;            /* number of one-byte codes */
  U32 ndict;        /* dictionary size */
  U8 unused[32];    /* bitmap of byte values absent from the input */
} PPHeader;

#define PP_HEADER_BYTES (1 + 1 + 4 + 32)

static int pp_isalpha(int c) { return (unsigned)((c | 32) - 'a') < 26; }
static int pp_isupper(int c) { return (unsigned)(c - 'A') < 26; }

/* byte values with each role, filled by pp_assign */
static U8 pp_capb, pp_allcapb, pp_entb[PP_NENT], pp_code1b[256], pp_prefb[256];
static int pp_nprefix;

/* Give roles to the free byte values, in ascending order. Returns the code
   capacity (how many dictionary words fit), or -1 if there are too few. */
static int pp_assign(const PPHeader *h) {
  memset(pp_role, 0, sizeof pp_role);
  memset(pp_arg, 0, sizeof pp_arg);
  pp_nprefix = 0;
  if (!h->feat) return 0;
  U8 u[256];
  int nu = 0;
  for (int c = 0; c < 256; ++c)
    if (h->unused[c >> 3] >> (c & 7) & 1) u[nu++] = (U8)c;
  int need = (h->feat & PP_FEAT_CAPS ? 2 : 0) + (h->feat & PP_FEAT_ENT ? PP_NENT : 0) +
             (h->feat & PP_FEAT_DICT ? h->k1 + 1 : 0);
  if (nu < need) return -1;
  int j = 0;
  if (h->feat & PP_FEAT_CAPS) {
    pp_capb = u[j]; pp_role[u[j++]] = R_CAP;
    pp_allcapb = u[j]; pp_role[u[j++]] = R_ALLCAP;
  }
  if (h->feat & PP_FEAT_ENT)
    for (int e = 0; e < PP_NENT; ++e) { pp_entb[e] = u[j]; pp_role[u[j]] = R_ENT; pp_arg[u[j++]] = (U8)e; }
  if (!(h->feat & PP_FEAT_DICT)) return 0;
  for (int i = 0; i < h->k1; ++i) { pp_code1b[i] = u[j]; pp_role[u[j]] = R_CODE1; pp_arg[u[j++]] = (U8)i; }
  for (; j < nu; ++j) { pp_prefb[pp_nprefix] = u[j]; pp_role[u[j]] = R_PREFIX; pp_arg[u[j]] = (U8)pp_nprefix++; }
  return h->k1 + pp_nprefix * PP_B2;
}

static void pp_header_write(const PPHeader *h, FILE *f) {
  putc_unlocked(h->feat, f);
  putc_unlocked(h->k1, f);
  for (int i = 3; i >= 0; --i) putc_unlocked((int)(h->ndict >> (8 * i)) & 255, f);
  fwrite(h->unused, 1, 32, f);
}

static int pp_header_read(PPHeader *h, FILE *f) {
  memset(h, 0, sizeof *h);
  int a = getc_unlocked(f), b = getc_unlocked(f);
  if (a == EOF || b == EOF) return -1;
  h->feat = (U8)a; h->k1 = (U8)b;
  for (int i = 0; i < 4; ++i) h->ndict = (h->ndict << 8) | (U32)getc_unlocked(f);
  if (fread(h->unused, 1, 32, f) != 32) return -1;
  return pp_assign(h) < 0 ? -1 : 0;
}

/* ---------- inverse: T -> original, streamed through a sink ---------- */

typedef int (*PPSink)(void *ctx, const U8 *s, size_t n); /* nonzero = stop */

typedef struct { U8 buf[1 << 16]; size_t n; PPSink sink; void *ctx; int stop; } PPOut;

static void pp_flush(PPOut *o) {
  if (o->n && !o->stop) o->stop = o->sink(o->ctx, o->buf, o->n);
  o->n = 0;
}
static inline void pp_out(PPOut *o, U8 c) {
  o->buf[o->n++] = c;
  if (o->n == sizeof o->buf) pp_flush(o);
}

/* Returns 0 on success, -1 if T is malformed or the sink stopped early. */
static int pp_inverse(const U8 *t, U64 m, const PPHeader *h, PPSink sink, void *ctx) {
  static PPOut o;
  o.n = 0; o.sink = sink; o.ctx = ctx; o.stop = 0;
  if (!h->feat) {
    for (U64 i = 0; i < m;) {
      size_t k = m - i < (1 << 20) ? (size_t)(m - i) : (1 << 20);
      if (sink(ctx, t + i, k)) return -1;
      i += k;
    }
    return 0;
  }
  U64 i = 0;
  U64 *woff = NULL;
  U8 *wlen = NULL;
  if (h->ndict) {
    woff = malloc(h->ndict * sizeof *woff);
    wlen = malloc(h->ndict);
    if (!woff || !wlen) { free(woff); free(wlen); return -1; }
    for (U32 k = 0; k < h->ndict; ++k) {
      U64 s = i;
      while (i < m && t[i] != '\n') ++i;
      if (i >= m || i - s > 255) { free(woff); free(wlen); return -1; }
      woff[k] = s; wlen[k] = (U8)(i - s);
      ++i;
    }
  }
  int pend = 0; /* 1 capitalise the next letter, 2 capitalise the letter run */
  int rc = 0;
  for (; i < m && !o.stop; ++i) {
    U8 c = t[i];
    switch (pp_role[c]) {
    case R_CAP: pend = 1; continue;
    case R_ALLCAP: pend = 2; continue;
    case R_ENT:
      for (int k = 0; k < pp_entlen[pp_arg[c]]; ++k) pp_out(&o, (U8)pp_ent[pp_arg[c]][k]);
      pend = 0;
      continue;
    case R_CODE1:
    case R_PREFIX: {
      U64 ci;
      if (pp_role[c] == R_CODE1) ci = pp_arg[c];
      else {
        if (i + 1 >= m || t[i + 1] < 0x80) { rc = -1; goto done; }
        ci = h->k1 + (U64)pp_arg[c] * PP_B2 + (t[++i] - 0x80);
      }
      if (ci >= h->ndict) { rc = -1; goto done; }
      for (int k = 0; k < wlen[ci]; ++k) {
        U8 w = t[woff[ci] + k];
        if (pend == 2 || (pend == 1 && k == 0)) w = (U8)(w - 32);
        pp_out(&o, w);
      }
      pend = 0;
      continue;
    }
    default:
      if (pp_isalpha(c)) {
        if (pend) { c = (U8)(c & ~32); if (pend == 1) pend = 0; }
      } else pend = 0;
      pp_out(&o, c);
    }
  }
done:
  pp_flush(&o);
  free(woff); free(wlen);
  return rc || o.stop ? -1 : 0;
}

/* ---------- forward: original -> T ---------- */

typedef struct { U32 pos, cnt; int code; U8 len; } PPWord;

static const U8 *pp_a; /* input, for the sort comparators */

static U32 pp_hashw(const U8 *s, int len) {
  U32 h = 2166136261u;
  for (int i = 0; i < len; ++i) h = (h ^ (U32)(s[i] | 32)) * 16777619u;
  return h ^ (h >> 15);
}

static int pp_eqw(const U8 *a, const U8 *b, int len) {
  for (int i = 0; i < len; ++i) if ((a[i] | 32) != (b[i] | 32)) return 0;
  return 1;
}

static PPWord *pp_tab;
static U32 pp_tmask, pp_tfill;

/* find or (if room) insert the case-folded word a[pos..pos+len) */
static PPWord *pp_find(U64 pos, int len, int insert) {
  U32 h = pp_hashw(pp_a + pos, len);
  for (U32 k = h & pp_tmask;; k = (k + 1) & pp_tmask) {
    PPWord *w = &pp_tab[k];
    if (!w->len) {
      if (!insert || pp_tfill >= pp_tmask / 4 * 3) return NULL;
      w->pos = (U32)pos; w->len = (U8)len; w->code = -1; ++pp_tfill;
      return w;
    }
    if (w->len == len && pp_eqw(pp_a + w->pos, pp_a + pos, len)) return w;
  }
}

/* 0 lower, 1 first upper rest lower, 2 all upper (len >= 2), 3 mixed */
static int pp_case(const U8 *s, int len) {
  int up0 = pp_isupper(s[0]), nup = 0;
  for (int i = 1; i < len; ++i) nup += pp_isupper(s[i]);
  if (!up0) return nup ? 3 : 0;
  if (nup == 0) return 1;
  return nup == len - 1 ? 2 : 3;
}

static int pp_cmp_score(const void *x, const void *y) {
  const PPWord *a = *(PPWord *const *)x, *b = *(PPWord *const *)y;
  U64 sa = (U64)a->cnt * (U64)(a->len - 1), sb = (U64)b->cnt * (U64)(b->len - 1);
  if (sa != sb) return sa > sb ? -1 : 1;
  return a->pos < b->pos ? -1 : 1;
}
static int pp_cmp_alpha(const void *x, const void *y) {
  const PPWord *a = *(PPWord *const *)x, *b = *(PPWord *const *)y;
  int n = a->len < b->len ? a->len : b->len;
  for (int i = 0; i < n; ++i) {
    int ca = pp_a[a->pos + i] | 32, cb = pp_a[b->pos + i] | 32;
    if (ca != cb) return ca - cb;
  }
  return a->len - b->len;
}
static int pp_cmp_suffix(const void *x, const void *y) {
  const PPWord *a = *(PPWord *const *)x, *b = *(PPWord *const *)y;
  int n = a->len < b->len ? a->len : b->len;
  for (int i = 1; i <= n; ++i) {
    int ca = pp_a[a->pos + a->len - i] | 32, cb = pp_a[b->pos + b->len - i] | 32;
    if (ca != cb) return ca - cb;
  }
  return a->len - b->len;
}

/* Emits T (when t != NULL) and returns its length. */
static U64 pp_emit(const U8 *a, U64 n, U8 *t, const PPHeader *h, PPWord **codes) {
  U64 m = 0;
#define PUT(c) do { U8 v_ = (U8)(c); if (t) t[m] = v_; ++m; } while (0) /* c evaluated once, always */
  for (U32 k = 0; k < h->ndict; ++k) {
    for (int i = 0; i < codes[k]->len; ++i) PUT(a[codes[k]->pos + i] | 32);
    PUT('\n');
  }
  int caps = h->feat & PP_FEAT_CAPS, ent = h->feat & PP_FEAT_ENT, dict = h->feat & PP_FEAT_DICT;
  for (U64 i = 0; i < n;) {
    U8 c = a[i];
    if (ent && c == '&') {
      int e = 0;
      for (; e < PP_NENT; ++e)
        if (i + pp_entlen[e] <= n && !memcmp(a + i, pp_ent[e], pp_entlen[e])) break;
      if (e < PP_NENT) { PUT(pp_entb[e]); i += pp_entlen[e]; continue; }
    }
    if (!pp_isalpha(c)) { PUT(c); ++i; continue; }
    U64 j = i;
    while (j < n && pp_isalpha(a[j])) ++j;
    int len = j - i > 255 ? 256 : (int)(j - i);
    int cls = len > 255 ? 3 : pp_case(a + i, len);
    if (cls == 3 || (cls && !caps)) { while (i < j) PUT(a[i++]); continue; }
    if (cls == 1) PUT(pp_capb);
    if (cls == 2) PUT(pp_allcapb);
    PPWord *w = dict && len >= PP_MINLEN ? pp_find(i, len, 0) : NULL;
    if (w && w->code >= 0) {
      U32 ci = (U32)w->code;
      if (ci < h->k1) PUT(pp_code1b[ci]);
      else { ci -= h->k1; PUT(pp_prefb[ci / PP_B2]); PUT(0x80 + ci % PP_B2); }
      i = j;
    } else {
      while (i < j) PUT(a[i++] | 32);
    }
  }
#undef PUT
  return m;
}

typedef struct { const U8 *a; U64 n, pos; } PPCheck;
static int pp_check_sink(void *ctx, const U8 *s, size_t k) {
  PPCheck *c = ctx;
  if (c->pos + k > c->n || memcmp(c->a + c->pos, s, k)) return 1;
  c->pos += k;
  return 0;
}

/* Transforms a[0..n) into a new buffer, or returns NULL with h->feat = 0 when
   the transform is off (the caller then codes the input as it is). */
static U8 *pp_forward(const U8 *a, U64 n, U64 *m_out, PPHeader *h) {
  memset(h, 0, sizeof *h);
  pp_assign(h);
  if (n < 64 || n >= 0xffffffffull) return NULL;
  U64 hist[256] = {0};
  for (U64 i = 0; i < n; ++i) hist[a[i]]++;
  for (int c = 0; c < 256; ++c) if (!hist[c]) h->unused[c >> 3] |= (U8)(1 << (c & 7));
  h->feat = (PP_CAPS ? PP_FEAT_CAPS : 0) | (PP_ENT ? PP_FEAT_ENT : 0) | (PP_DICT ? PP_FEAT_DICT : 0);
  h->k1 = PP_DICT ? PP_K1 : 0;
  int capacity = pp_assign(h);
  if (capacity < 0) { memset(h, 0, sizeof *h); pp_assign(h); return NULL; }

  PPWord **codes = NULL;
  pp_a = a;
  pp_tab = NULL;
  if (h->feat & PP_FEAT_DICT) {
    pp_tmask = (1u << PP_HBITS) - 1;
    pp_tfill = 0;
    pp_tab = calloc((size_t)1 << PP_HBITS, sizeof *pp_tab);
    if (!pp_tab) { memset(h, 0, sizeof *h); pp_assign(h); return NULL; }
    for (U64 i = 0; i < n;) {
      if (!pp_isalpha(a[i])) { ++i; continue; }
      U64 j = i;
      while (j < n && pp_isalpha(a[j])) ++j;
      int len = j - i > 255 ? 256 : (int)(j - i);
      int cls = len > 255 ? 3 : pp_case(a + i, len);
      if (len >= PP_MINLEN && cls != 3 && (cls == 0 || (h->feat & PP_FEAT_CAPS))) {
        PPWord *w = pp_find(i, len, 1);
        if (w) w->cnt++;
      }
      i = j;
    }
    U32 nc = 0;
    for (U32 k = 0; k <= pp_tmask; ++k)
      if (pp_tab[k].len && pp_tab[k].cnt >= PP_MINCOUNT) ++nc;
    codes = malloc((nc ? nc : 1) * sizeof *codes);
    nc = 0;
    for (U32 k = 0; k <= pp_tmask; ++k)
      if (pp_tab[k].len && pp_tab[k].cnt >= PP_MINCOUNT) codes[nc++] = &pp_tab[k];
    qsort(codes, nc, sizeof *codes, pp_cmp_score);
    U32 nd = nc < (U32)capacity ? nc : (U32)capacity;
    if (nd > PP_MAXDICT) nd = PP_MAXDICT;
    U32 k1 = nd < h->k1 ? nd : h->k1;
    h->k1 = (U8)k1;
    if (PP_SORT == 1) qsort(codes + k1, nd - k1, sizeof *codes, pp_cmp_alpha);
    if (PP_SORT == 2) qsort(codes + k1, nd - k1, sizeof *codes, pp_cmp_suffix);
    for (U32 k = 0; k < nd; ++k) codes[k]->code = (int)k;
    h->ndict = nd;
    pp_assign(h); /* k1 may have shrunk */
  }

  U64 m = pp_emit(a, n, NULL, h, codes);
  U8 *t = malloc(m ? m : 1);
  if (t) pp_emit(a, n, t, h, codes);
  free(codes);
  free(pp_tab);
  pp_tab = NULL;
  if (!t) { memset(h, 0, sizeof *h); pp_assign(h); return NULL; }

  PPCheck chk = {a, n, 0};
  if (pp_inverse(t, m, h, pp_check_sink, &chk) || chk.pos != n) {
    fprintf(stderr, "preproc: self-check failed, coding without the transform\n");
    free(t);
    memset(h, 0, sizeof *h);
    pp_assign(h);
    return NULL;
  }
  *m_out = m;
  return t;
}
