/*
 * cm2 - context-mixing compressor, the second attempt. Starts as a copy of cm1;
 *       each step is added behind a compile-time flag (see README.md).
 *
 *   cm2 c <input> <archive>     compress
 *   cm2 d <archive> <output>    decompress
 *
 * Build with -DCOST_LOG to also write <archive>.cost during compression: one
 * float32 per input byte, the bits spent coding it (sum of -log2 p over its 8
 * bits). The archive itself is unchanged. tools/bitcost reads this file.
 *
 * How it works (the PAQ recipe, cut down to the essentials):
 *   1. The file is coded one bit at a time with a binary arithmetic coder.
 *   2. For every bit, a set of models each predict P(bit = 1) from a different
 *      context: order-1, 2, 3, 4, 6 and 8 byte contexts, the current word, the current word
 *      plus the previous one, and a long-range match model.
 *   3. A small neural network (logistic mixing) combines those predictions,
 *      with a weight set chosen by the partial byte and the match length.
 *   4. An APM (secondary estimation) refines the mixed probability using the
 *      order-1 context.
 *   5. Every component learns online from the bit just coded, so the
 *      decompressor rebuilds exactly the same model from the data alone.
 *
 * Context tables are hashed into 64-byte blocks of 16 counters, one block per
 * nibble, so each model costs about two cache misses per byte.
 *
 * This Code is licensed under UNLICENSE http://unlicense.org
 */
#define _POSIX_C_SOURCE 200809L /* getc_unlocked: plain getc locks per byte */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t U8;
typedef uint16_t U16;
typedef uint32_t U32;
typedef uint64_t U64;

/* Step 1: bit-history states instead of plain probability counters (on by default). */
#ifndef USE_BITHIST
#define USE_BITHIST 1
#endif

#if USE_BITHIST
#define TABLE_BITS 24       /* 2^24 blocks x 16 bytes = 256 MB per context model */
#ifndef RUN_INPUT
#define RUN_INPUT 1         /* also feed a run input per model */
#endif
#define INPUTS_PER_CTX (1 + RUN_INPUT)
#ifndef SM_LIMIT
#define SM_LIMIT 1023       /* StateMap adaptation limit */
#endif
#else
#define TABLE_BITS 22       /* 2^22 blocks x 64 bytes = 256 MB per context model */
#define INPUTS_PER_CTX 1
#endif
#define MATCH_HASH_BITS 24  /* 64 MB of match pointers */
#define MATCH_MIN 7         /* bytes of context hashed to find a match */
#define N_CTX 8             /* hashed context models */
#define N_INPUTS (N_CTX * INPUTS_PER_CTX + 2) /* + match model + bias */
#ifndef MIXER_LR
#define MIXER_LR 4          /* mixer learning rate; swept 1..6 on enwik7, 4 was best */
#endif

static void *xcalloc(size_t n, size_t sz) {
  void *p = calloc(n, sz);
  if (!p) { fprintf(stderr, "out of memory (%zu bytes)\n", n * sz); exit(2); }
  return p;
}

/* ---------- logistic helpers: squash = 1/(1+e^-x), stretch = its inverse ---------- */

static int squash(int d) { /* d in 1/256 units, returns 12-bit probability */
  static const int t[33] = {1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101, 1546,
                            2047, 2549, 2994, 3348, 3607, 3785, 3901, 3975, 4022, 4050, 4068,
                            4079, 4085, 4089, 4092, 4093, 4094};
  if (d > 2047) return 4095;
  if (d < -2047) return 1;
  int w = d & 127;
  d = (d >> 7) + 16;
  return (t[d] * (128 - w) + t[d + 1] * w + 64) >> 7;
}

static short stretch_t[4096];
static int stretch(int p) { return stretch_t[p]; }

static void init_stretch(void) {
  int pi = 0;
  for (int x = -2047; x <= 2047; ++x) {
    int v = squash(x);
    for (int j = pi; j <= v; ++j) stretch_t[j] = (short)x;
    pi = v + 1;
  }
  for (int j = pi; j < 4096; ++j) stretch_t[j] = 2047;
}

/* ---------- adaptive probability counter ----------
 * 32 bits: high 22 = probability, low 10 = hit count. The learning rate is
 * 1/(n+1.5), so a new context learns fast and an old one settles. */

static int dt[1024];

static void init_dt(void) {
  for (int i = 0; i < 1024; ++i) dt[i] = 16384 / (i + i + 3);
}

static inline int counter_p(U32 t) { return t >> 20; }

static inline void counter_update(U32 *t, int y, int limit) {
  U32 v = *t;
  int n = v & 1023, p = v >> 10;
  if (n < limit) ++v;
  else v = (v & 0xfffffc00) | limit;
  v += (((y << 22) - p) >> 3) * dt[n] & 0xfffffc00;
  *t = v;
}

#if USE_BITHIST
/* ---------- bit-history states (step 1) ----------
 * A context slot holds one byte: a state standing for (n0, n1, last bit), the
 * recent counts of zeros and ones seen in that context. On each new bit the
 * matching count goes up and a large opposite count is cut to about half, so
 * the state favours recent behaviour. Counts are bounded so that every state
 * fits in a byte: the smaller count may be up to 6 and the larger one shrinks
 * as the smaller grows (lim[] below), giving 237 states. Whether the last bit
 * was 0 or 1 is kept only when both counts are non-zero.
 *
 * A StateMap per model then learns which probability each state really
 * stands for, so "seen 1,1,0,1" is mapped by experience rather than by a
 * fixed formula. The table is generated at startup, deterministically. */

static U8 nex_t[256][2];       /* next state after bit 0 / bit 1 */
static U8 st_n0[256], st_n1[256];
static int n_states;

static int st_allowed(int a, int b) {
  static const int lim[7] = {30, 24, 12, 8, 6, 6, 6};
  int lo = a < b ? a : b, hi = a < b ? b : a;
  return lo <= 6 && hi <= lim[lo];
}

static void init_states(void) {
  static short idx[31][31][2];
  memset(idx, -1, sizeof idx);
  n_states = 1; /* state 0 = never seen (0, 0) */
  idx[0][0][0] = idx[0][0][1] = 0;
  for (int sum = 1; sum <= 60; ++sum)
    for (int a = 0; a <= 30; ++a) {
      int b = sum - a;
      if (b < 0 || b > 30 || !st_allowed(a, b)) continue;
      for (int last = 0; last < 2; ++last) {
        if ((a == 0 || b == 0) && last != (a == 0)) { /* last bit is implied */
          continue;
        }
        st_n0[n_states] = (U8)a;
        st_n1[n_states] = (U8)b;
        idx[a][b][last] = (short)n_states++;
      }
      if (a == 0 || b == 0) idx[a][b][!(a == 0)] = idx[a][b][a == 0];
    }
  for (int s = 0; s < n_states; ++s)
    for (int y = 0; y < 2; ++y) {
      int a = st_n0[s], b = st_n1[s];
      if (y) { ++b; if (a > 2) a = a / 2 + 1; }
      else { ++a; if (b > 2) b = b / 2 + 1; }
      while (!st_allowed(a, b)) {
        if (y ? b >= a : a >= b) { if (y) --b; else --a; } /* saturate the larger count */
        else { if (y) --a; else --b; }
      }
      nex_t[s][y] = (U8)idx[a][b][y];
    }
}

/* StateMap entry: a counter (see above) initialised from the state's counts */
static U32 statemap_init(int s) {
  int n0 = st_n0[s], n1 = st_n1[s];
  U32 p22 = (U32)(((U64)(2 * n1 + 1) << 22) / (U64)(2 * (n0 + n1) + 2));
  return p22 << 10;
}
#endif

/* ---------- mixer: one weight set per selector, inputs are stretched probabilities ---------- */

typedef struct {
  int x[N_INPUTS];
  int *w;
  int sel, pr;
} Mixer;

static void mixer_init(Mixer *m, int nsel) {
  m->w = xcalloc((size_t)nsel * N_INPUTS, sizeof(int));
  /* Each model's main input starts at weight 1/4; extra inputs start at 0. */
  for (int i = 0; i < nsel * N_INPUTS; ++i) {
    int j = i % N_INPUTS;
    int extra = j < N_CTX * INPUTS_PER_CTX && j % INPUTS_PER_CTX != 0;
    m->w[i] = extra ? 0 : (1 << 16) / 4;
  }
}

static int mixer_p(Mixer *m, int sel) {
  m->sel = sel * N_INPUTS;
  int64_t dot = 0;
  for (int i = 0; i < N_INPUTS; ++i) dot += (int64_t)m->x[i] * m->w[m->sel + i];
  int d = (int)(dot >> 16);
  if (d > 2047) d = 2047;
  if (d < -2047) d = -2047;
  return m->pr = squash(d);
}

static void mixer_update(Mixer *m, int y) {
  int err = ((y << 12) - m->pr) * MIXER_LR;
  int *w = m->w + m->sel;
  for (int i = 0; i < N_INPUTS; ++i) w[i] += (m->x[i] * err + (1 << 15)) >> 16;
}

/* ---------- APM: maps (probability, context) to a refined probability ---------- */

typedef struct {
  U16 *t;
  int idx;
} Apm;

static void apm_init(Apm *a, int n) {
  a->t = xcalloc((size_t)n * 24, sizeof(U16));
  for (int i = 0; i < n * 24; ++i) a->t[i] = (U16)(squash((i % 24 * 2 + 1) * 4096 / 48 - 2048) * 16);
}

static int apm_p(Apm *a, int pr, int cx) {
  pr = (stretch(pr) + 2048) * 23;
  int wt = pr & 0xfff;
  cx = cx * 24 + (pr >> 12);
  a->idx = cx + (wt >> 11);
  return (a->t[cx] * (4096 - wt) + a->t[cx + 1] * wt) >> 16;
}

static void apm_update(Apm *a, int y, int rate) {
  int g = (y << 16) + (y << rate) - y - y;
  a->t[a->idx] += (g - a->t[a->idx]) >> rate;
}

/* ---------- hashing ---------- */

static inline U32 hash2(U32 a, U32 b) {
  U32 h = a * 0x9E3779B1u ^ b * 0x85EBCA6Bu;
  h ^= h >> 15;
  h *= 0xC2B2AE35u;
  return h ^ (h >> 13);
}

/* ---------- the predictor ---------- */

#if USE_BITHIST
typedef U8 Slot;       /* a bit-history state */
#else
typedef U32 Slot;      /* a probability counter */
#endif

typedef struct {
  /* context models: blocks of 16 slots, one block per context and nibble */
  Slot *table[N_CTX];
  U32 ctxhash[N_CTX];  /* per-byte context hash */
  Slot *slot[N_CTX];   /* current block */
  int limit[N_CTX];
#if USE_BITHIST
  U32 sm[N_CTX][256];  /* StateMap per model: state -> probability */
  int st[N_CTX];       /* state used for the current bit */
#endif

  /* history */
  U8 *buf;
  U64 pos, cap;
  U32 c0;              /* partial byte with leading 1 */
  int bitpos;
  U32 c4, c8;          /* last 8 bytes */
  U32 word, prevword;

  /* match model */
  U32 *mtab;
  U64 mptr;
  int mlen, mbit;
  U32 msm[64 * 2];

  Mixer mx;
  Apm a1, a2;
  int pr_mix, pr;
} Predictor;

static void predictor_init(Predictor *P, U64 cap) {
  memset(P, 0, sizeof *P);
  for (int i = 0; i < N_CTX; ++i) {
    P->table[i] = xcalloc((size_t)16 << TABLE_BITS, sizeof(Slot));
    P->limit[i] = i < 2 ? 1023 : 255;
#if USE_BITHIST
    for (int s = 0; s < 256; ++s) P->sm[i][s] = s < n_states ? statemap_init(s) : 1u << 31;
#endif
  }
  for (int i = 0; i < 64 * 2; ++i) P->msm[i] = 1u << 31;
  P->cap = cap ? cap : 1;
  P->buf = xcalloc(P->cap, 1);
  P->mtab = xcalloc((size_t)1 << MATCH_HASH_BITS, sizeof(U32));
  P->c0 = 1;
  mixer_init(&P->mx, 256 * 4);
  apm_init(&P->a1, 256);
  apm_init(&P->a2, 1 << 16);
}

/* choose the 16-counter block for each model; called at each nibble boundary */
static void select_slots(Predictor *P) {
  U32 nib = P->bitpos == 0 ? 0 : P->c0; /* 0 at byte start, 16..31 at mid byte */
  for (int i = 0; i < N_CTX; ++i) {
    U32 h = hash2(P->ctxhash[i], nib + 1);
    P->slot[i] = P->table[i] + ((size_t)(h >> (32 - TABLE_BITS)) << 4);
#if USE_BITHIST
    /* Slot 0 is never used for a bit (positions are 1..15), so it holds an
       8-bit check of the context. A mismatch means another context owns the
       block: start it fresh rather than reuse its histories. */
    U8 check = (U8)(h & 255);
    if (P->slot[i][0] != check) { memset(P->slot[i], 0, 16); P->slot[i][0] = check; }
#endif
  }
}

static void byte_update(Predictor *P, int c) {
  P->buf[P->pos % P->cap] = (U8)c;
  P->pos++;
  P->c8 = (P->c8 << 8) | (P->c4 >> 24);
  P->c4 = (P->c4 << 8) | (U32)c;

  /* words: letters only, case folded */
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
    P->word = hash2(P->word, (U32)(c | 32));
  } else if (P->word) {
    P->prevword = P->word;
    P->word = 0;
  }

  P->ctxhash[0] = hash2(1, P->c4 & 0xff);
  P->ctxhash[1] = hash2(2, P->c4 & 0xffff);
  P->ctxhash[2] = hash2(3, P->c4 & 0xffffff);
  P->ctxhash[3] = hash2(4, P->c4);
  P->ctxhash[4] = hash2(hash2(5, P->c4), P->c8 & 0xffff);
  P->ctxhash[5] = hash2(hash2(6, P->c4), P->c8);
  P->ctxhash[6] = hash2(hash2(7, P->word), P->c4 & 0xff);
  P->ctxhash[7] = hash2(hash2(8, P->word), P->prevword);

  /* match model: extend the current match or look up a new one */
  if (P->mlen > 0 && P->buf[P->mptr % P->cap] == (U8)c) {
    P->mlen++;
    P->mptr++;
  } else {
    P->mlen = 0;
  }
  if (P->pos >= MATCH_MIN) {
    U32 h = 0;
    for (int i = 1; i <= MATCH_MIN; ++i) h = hash2(h, P->buf[(P->pos - i) % P->cap]);
    h >>= 32 - MATCH_HASH_BITS;
    if (P->mlen == 0) {
      U64 cand = P->mtab[h];
      if (cand > 0) {
        int len = 0;
        while (len < 32 && cand > (U64)len &&
               P->buf[(cand - 1 - len) % P->cap] == P->buf[(P->pos - 1 - len) % P->cap])
          ++len;
        if (len >= MATCH_MIN) { P->mlen = len; P->mptr = cand; }
      }
    }
    P->mtab[h] = (U32)P->pos;
  }
}

static int predict(Predictor *P) {
  Mixer *m = &P->mx;
  int k = (int)P->c0; /* position inside the 16-counter block: 1..15 */
  if (P->bitpos >= 4) k = (int)((P->c0 & ((1u << (P->bitpos - 4)) - 1)) | (1u << (P->bitpos - 4)));
#if USE_BITHIST
  for (int i = 0; i < N_CTX; ++i) {
    int s = P->st[i] = P->slot[i][k];
    int st = stretch(counter_p(P->sm[i][s]));
    m->x[INPUTS_PER_CTX * i] = st;
#if RUN_INPUT
    /* Run input: a context that has only ever seen one bit value is a strong
       hint, more so the longer the run. Zero when the history is mixed. */
    int n0 = st_n0[s], n1 = st_n1[s];
    m->x[2 * i + 1] = n0 == 0 && n1 > 0 ? 64 * (n1 < 16 ? n1 : 16)
                    : n1 == 0 && n0 > 0 ? -64 * (n0 < 16 ? n0 : 16) : 0;
#endif
  }
#else
  for (int i = 0; i < N_CTX; ++i) m->x[i] = stretch(counter_p(P->slot[i][k]));
#endif

  /* match model input */
  int lenb = 0;
  P->mbit = -1;
  if (P->mlen > 0) {
    int expected = P->buf[P->mptr % P->cap] | 256;
    if ((U32)(expected >> (8 - P->bitpos)) == P->c0) {
      P->mbit = (expected >> (7 - P->bitpos)) & 1;
      int l = P->mlen < 63 ? P->mlen : 63;
      lenb = l < 16 ? 1 : l < 32 ? 2 : 3;
      m->x[N_INPUTS - 2] = stretch(counter_p(P->msm[l * 2 + P->mbit]));
    } else {
      P->mlen = 0;
      m->x[N_INPUTS - 2] = 0;
    }
  } else {
    m->x[N_INPUTS - 2] = 0;
  }
  m->x[N_INPUTS - 1] = 256;

  P->pr_mix = mixer_p(m, (int)P->c0 + 256 * lenb);
  int p1 = apm_p(&P->a1, P->pr_mix, (int)P->c0);
  int p2 = apm_p(&P->a2, P->pr_mix, (int)(P->c0 | ((P->c4 & 0xff) << 8)));
  int pr = (P->pr_mix * 2 + p1 + p2 + 2) >> 2;
  if (pr < 1) pr = 1;
  if (pr > 4095) pr = 4095;
  return P->pr = pr;
}

static void update(Predictor *P, int y) {
  int k = (int)P->c0;
  if (P->bitpos >= 4) k = (int)((P->c0 & ((1u << (P->bitpos - 4)) - 1)) | (1u << (P->bitpos - 4)));
#if USE_BITHIST
  for (int i = 0; i < N_CTX; ++i) {
    counter_update(&P->sm[i][P->st[i]], y, SM_LIMIT);
    P->slot[i][k] = nex_t[P->st[i]][y];
  }
  (void)k;
#else
  for (int i = 0; i < N_CTX; ++i) counter_update(&P->slot[i][k], y, P->limit[i]);
#endif
  if (P->mbit >= 0) {
    int l = P->mlen < 63 ? P->mlen : 63;
    counter_update(&P->msm[l * 2 + P->mbit], y, 1023);
  }
  mixer_update(&P->mx, y);
  apm_update(&P->a1, y, 7);
  apm_update(&P->a2, y, 7);

  P->c0 = (P->c0 << 1) | (U32)y;
  P->bitpos++;
  if (P->bitpos == 8) {
    byte_update(P, (int)(P->c0 & 255));
    P->c0 = 1;
    P->bitpos = 0;
    select_slots(P);
  } else if (P->bitpos == 4) {
    select_slots(P);
  }
}

/* ---------- binary arithmetic coder (carryless, 32-bit) ---------- */

typedef struct {
  U32 x1, x2, x;
  FILE *f;
} Coder;

static void encode_bit(Coder *c, int p, int y) {
  U32 xmid = c->x1 + (U32)(((U64)(c->x2 - c->x1) * (U32)p) >> 12);
  if (y) c->x2 = xmid;
  else c->x1 = xmid + 1;
  while (((c->x1 ^ c->x2) & 0xff000000) == 0) {
    putc_unlocked(c->x2 >> 24, c->f);
    c->x1 <<= 8;
    c->x2 = (c->x2 << 8) | 255;
  }
}

static int decode_bit(Coder *c, int p) {
  U32 xmid = c->x1 + (U32)(((U64)(c->x2 - c->x1) * (U32)p) >> 12);
  int y = c->x <= xmid;
  if (y) c->x2 = xmid;
  else c->x1 = xmid + 1;
  while (((c->x1 ^ c->x2) & 0xff000000) == 0) {
    c->x1 <<= 8;
    c->x2 = (c->x2 << 8) | 255;
    int b = getc_unlocked(c->f);
    c->x = (c->x << 8) | (U32)(b == EOF ? 0 : b);
  }
  return y;
}

/* ---------- driver ---------- */

#ifdef COST_LOG
/* -log2(p/4096) for p = 1..4095, without libm: ln(x) = 2 atanh((x-1)/(x+1)) */
static float cost_t[4096];
static void init_cost(void) {
  for (int p = 1; p < 4096; ++p) {
    double x = p / 4096.0, z = (x - 1) / (x + 1), z2 = z * z, term = z, sum = 0;
    for (int k = 1; k < 200; k += 2) { sum += term / k; term *= z2; }
    cost_t[p] = (float)(-2 * sum / 0.69314718055994530942);
  }
}
#endif

static void progress(U64 done, U64 total) {
  if (total >= 10000000 && done % 10000000 == 0)
    fprintf(stderr, "\r%3d%%", (int)(done * 100 / total)), fflush(stderr);
}

static int compress(const char *in, const char *out) {
  FILE *fi = fopen(in, "rb"), *fo = fopen(out, "wb");
  if (!fi || !fo) { perror("open"); return 1; }
  fseek(fi, 0, SEEK_END);
  U64 n = (U64)ftell(fi);
  fseek(fi, 0, SEEK_SET);
  for (int i = 7; i >= 0; --i) putc_unlocked((int)(n >> (i * 8)) & 255, fo);

  static Predictor P;
  predictor_init(&P, n);
  select_slots(&P);
  Coder c = {0, 0xffffffff, 0, fo};
#ifdef COST_LOG
  init_cost();
  char cost_name[4096];
  snprintf(cost_name, sizeof cost_name, "%s.cost", out);
  FILE *fc = fopen(cost_name, "wb");
  if (!fc) { perror("open cost log"); return 1; }
#endif
  for (U64 i = 0; i < n; ++i) {
    int ch = getc_unlocked(fi);
#ifdef COST_LOG
    float bits = 0;
#endif
    for (int b = 7; b >= 0; --b) {
      int y = (ch >> b) & 1;
      int p = predict(&P);
#ifdef COST_LOG
      bits += cost_t[y ? p : 4096 - p];
#endif
      encode_bit(&c, p, y);
      update(&P, y);
    }
#ifdef COST_LOG
    fwrite(&bits, sizeof bits, 1, fc);
#endif
    progress(i + 1, n);
  }
#ifdef COST_LOG
  fclose(fc);
#endif
  for (int i = 0; i < 4; ++i) { putc_unlocked(c.x1 >> 24, fo); c.x1 <<= 8; }
  fprintf(stderr, "\r%llu -> %ld bytes\n", (unsigned long long)n, ftell(fo));
  fclose(fi);
  fclose(fo);
  return 0;
}

static int decompress(const char *in, const char *out) {
  FILE *fi = fopen(in, "rb"), *fo = fopen(out, "wb");
  if (!fi || !fo) { perror("open"); return 1; }
  U64 n = 0;
  for (int i = 0; i < 8; ++i) n = (n << 8) | (U64)getc(fi);

  static Predictor P;
  predictor_init(&P, n);
  select_slots(&P);
  Coder c = {0, 0xffffffff, 0, fi};
  for (int i = 0; i < 4; ++i) { int b = getc_unlocked(fi); c.x = (c.x << 8) | (U32)(b == EOF ? 0 : b); }
  for (U64 i = 0; i < n; ++i) {
    int ch = 0;
    for (int b = 0; b < 8; ++b) {
      int y = decode_bit(&c, predict(&P));
      update(&P, y);
      ch = (ch << 1) | y;
    }
    putc_unlocked(ch, fo);
    progress(i + 1, n);
  }
  fprintf(stderr, "\rdecompressed %llu bytes\n", (unsigned long long)n);
  fclose(fi);
  fclose(fo);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 4 || (argv[1][0] != 'c' && argv[1][0] != 'd')) {
    fprintf(stderr, "usage: cm2 c|d <input> <output>\n");
    return 1;
  }
  init_stretch();
  init_dt();
#if USE_BITHIST
  init_states();
#endif
  return argv[1][0] == 'c' ? compress(argv[2], argv[3]) : decompress(argv[2], argv[3]);
}
