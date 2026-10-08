/*
 * cm2 - context-mixing compressor, the second attempt. Starts as a copy of cm1;
 *       each step is added behind a compile-time flag (see README.md).
 *
 *   cm2 c <input> <archive>     compress
 *   cm2 d <archive> <output>    decompress
 *
 * Build with -DCOST_LOG to also write <archive>.cost during compression: one
 * float32 per coded byte, the bits spent coding it (sum of -log2 p over its 8
 * bits). The archive itself is unchanged. tools/bitcost reads this file; it
 * lines up with the input only with -DUSE_PREPROC=0, because the step 4
 * transform changes the byte stream that gets coded.
 *
 * Step 4 (preproc.c): before modelling, the input goes through a reversible
 * transform (capital flags, entity bytes, a word dictionary built from the
 * input). The model codes the transformed stream; the decompressor decodes it
 * and runs the inverse.
 *
 * How it works (the PAQ recipe, cut down to the essentials):
 *   1. The file is coded one bit at a time with a binary arithmetic coder.
 *   2. For every bit, a set of models each predict P(bit = 1) from a different
 *      context: order-1, 2, 3, 4, 6 and 8 byte contexts, the current word, the current word
 *      plus the previous one, wiki structure contexts (step 3) and a
 *      long-range match model.
 *   3. A small neural network (logistic mixing) combines those predictions,
 *      with a weight set chosen by the partial byte, the match length and a
 *      coarse wiki parse state.
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

/* Step 4: reversible text transform in front of the model (preproc.c). */
#ifndef USE_PREPROC
#define USE_PREPROC 1
#endif
#if USE_PREPROC
#include "preproc.c"
#endif

/* Step 1: bit-history states instead of plain probability counters (on by default). */
#ifndef USE_BITHIST
#define USE_BITHIST 1
#endif

#if USE_BITHIST
/* Step 2: table size per context model is 2^TABLE_BITS blocks of 16 bytes.
 * 24 = 256 MB per model, about 1.7 GB in total (dev default, fits the laptop).
 * 26 = 1 GB per model, about 7.2 GB in total (release, under the 10 GB cap).
 * The order-1 and order-2 models have few contexts and get small tables. */
#ifndef TABLE_BITS
#define TABLE_BITS 24
#endif
/* Step 2: 4-way buckets. A context's block is looked up in a 64-byte bucket of
 * 4 blocks (one cache line), matched by its check byte; on a miss the block
 * with the least history is replaced. 0 = step 1 behaviour (one block per
 * hash, replaced on any mismatch). */
#ifndef USE_BUCKETS
#define USE_BUCKETS 1
#endif
#ifndef RUN_INPUT
#define RUN_INPUT 1         /* also feed a run input per model */
#endif
#define INPUTS_PER_CTX (1 + RUN_INPUT)
#ifndef SM_LIMIT
#define SM_LIMIT 511        /* StateMap adaptation limit (step 7: 511 beat 1023) */
#endif
#else
#define TABLE_BITS 22       /* 2^22 blocks x 64 bytes = 256 MB per context model */
#define INPUTS_PER_CTX 1
#endif
#define MATCH_HASH_BITS 24  /* 64 MB of match pointers */
#ifndef MATCH_MIN
#define MATCH_MIN 7         /* bytes of context hashed to find a match */
#endif
#ifndef APM_RATE
#define APM_RATE 7          /* APM adaptation shift (higher = slower) */
#endif
#ifndef MSM_LIMIT
#define MSM_LIMIT 255       /* match model StateMap adaptation limit (step 7) */
#endif
/* Step 3: extra context models built on a small wiki parse state (inside a
 * link or template, which part of it, table column, line type, numbers).
 * WIKICTX is a bitmask; each bit adds one hashed model (see byte_update):
 *   1 word + parse state     2 order-2 + parse state   4 line/table column
 *   8 number                16 sparse: bytes 2-3 back  32 word + word before previous
 * MIXSEL_PARSE=1 also picks the mixer weight set by a coarse parse state. */
#ifndef WIKICTX
#define WIKICTX 37          /* 1+4+32: best gain per time; see README for the others */
#endif
#ifndef MIXSEL_PARSE
#define MIXSEL_PARSE 1
#endif
#ifndef EXTRA_TABLE_BITS
#define EXTRA_TABLE_BITS 23 /* table size cap for the step 3 models */
#endif
/* Step 6: match model. MLONG=n (n >= 12) adds a second hash table keyed on the
 * last n bytes; a new match is looked up there first, so long repeats are
 * found even when the short hash slot was overwritten. MATCH_CTX adds a hashed
 * context model keyed on the byte the match predicts plus its length bucket
 * and the order-1 (1) or order-2 (2) context. MSM2 indexes the match StateMap
 * by (length up to 15, expected bit, recent misses 0..3). */
#ifndef MLONG
#define MLONG 0
#endif
#ifndef MLONG_BITS
#define MLONG_BITS 22
#endif
#ifndef MATCH_CTX
#define MATCH_CTX 2   /* step 6: match byte + length bucket + order-2, accepted on enwik8 */
#endif
#ifndef MSM2
#define MSM2 0
#endif
#define WBIT(n) (((WIKICTX) >> (n)) & 1)
#define N_EXTRA (WBIT(0) + WBIT(1) + WBIT(2) + WBIT(3) + WBIT(4) + WBIT(5) + (MATCH_CTX ? 1 : 0))
#define N_CTX (8 + N_EXTRA)  /* hashed context models */
#define N_INPUTS (N_CTX * INPUTS_PER_CTX + 2) /* + match model + bias */
#ifndef MIXER_LR
#define MIXER_LR 6          /* mixer learning rate; step 7 on enwik8: 6 beat 3, 4, 5, 8 */
#endif
/* Step 5: several first-layer mixers, each choosing its weight set by a
 * different context, combined by a small second-layer mixer (MIX2). MIX2_SETS
 * is a bitmask of the extra first-layer selectors (the step 3 one always runs):
 *   1 order-1 byte    2 match length    4 order-3/6 confidence    8 parse kind
 * APM_EXT adds two APMs (order-2 hashed, match state) to the final stage. */
#ifndef MIX2
#define MIX2 1
#endif
#ifndef MIX2_SETS
#define MIX2_SETS 4          /* confidence selector only: best gain per time on enwik8 */
#endif
#ifndef MIX2_LR
#define MIX2_LR 2
#endif
#ifndef APM_EXT
#define APM_EXT 1
#endif
#ifndef MIX2_O1BITS
#define MIX2_O1BITS 8      /* high bits of the previous byte used by the order-1 selector */
#endif
#ifndef APM_W0
#define APM_W0 0            /* weight of the mixer output against each APM in the final average */
#endif
#define MBIT(n) (((MIX2_SETS) >> (n)) & 1)
#define N_MIX1 (1 + (MIX2 ? MBIT(0) + MBIT(1) + MBIT(2) + MBIT(3) : 0))

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
  int sel, pr, dot;
} Mixer;

static int *mixer_weights(int nsel) {
  int *w = xcalloc((size_t)nsel * N_INPUTS, sizeof(int));
  /* Each model's main input starts at weight 1/4; extra inputs start at 0. */
  for (int i = 0; i < nsel * N_INPUTS; ++i) {
    int j = i % N_INPUTS;
    int extra = j < N_CTX * INPUTS_PER_CTX && j % INPUTS_PER_CTX != 0;
    w[i] = extra ? 0 : (1 << 16) / 4;
  }
  return w;
}

static void mixer_init(Mixer *m, int nsel) { m->w = mixer_weights(nsel); }

/* dot product of the shared inputs with one weight set, clamped, in stretch units */
static inline int mix_dot(const int *x, const int *w) {
  int64_t dot = 0;
  for (int i = 0; i < N_INPUTS; ++i) dot += (int64_t)x[i] * w[i];
  int d = (int)(dot >> 16);
  if (d > 2047) d = 2047;
  if (d < -2047) d = -2047;
  return d;
}

static inline void mix_train(const int *x, int *w, int err) {
  for (int i = 0; i < N_INPUTS; ++i) w[i] += (x[i] * err + (1 << 15)) >> 16;
}

static int mixer_p(Mixer *m, int sel) {
  m->sel = sel * N_INPUTS;
  m->dot = mix_dot(m->x, m->w + m->sel);
  return m->pr = squash(m->dot);
}

static void mixer_update(Mixer *m, int y) {
  mix_train(m->x, m->w + m->sel, ((y << 12) - m->pr) * MIXER_LR);
}

#if MIX2
/* Step 5: extra first-layer mixers share the inputs of the main one but pick
 * their own weight set; each learns from its own error, PAQ8 style. */
typedef struct {
  int *w;
  int sel, dot, pr;
} Mix1;

/* Second layer: mixes the first-layer outputs (in stretch units). */
typedef struct {
  int x[N_MIX1 + 1];
  int *w;
  int sel, pr;
} Mix2;

static void mix2_init(Mix2 *m, int nsel) {
  m->w = xcalloc((size_t)nsel * (N_MIX1 + 1), sizeof(int));
  for (int s = 0; s < nsel; ++s)
    for (int i = 0; i < N_MIX1; ++i) m->w[s * (N_MIX1 + 1) + i] = (1 << 16) / N_MIX1;
}

static int mix2_p(Mix2 *m, int sel) {
  m->sel = sel * (N_MIX1 + 1);
  int64_t dot = 0;
  for (int i = 0; i <= N_MIX1; ++i) dot += (int64_t)m->x[i] * m->w[m->sel + i];
  int d = (int)(dot >> 16);
  if (d > 2047) d = 2047;
  if (d < -2047) d = -2047;
  return m->pr = squash(d);
}

static void mix2_update(Mix2 *m, int y) {
  int err = ((y << 12) - m->pr) * MIX2_LR;
  int *w = m->w + m->sel;
  for (int i = 0; i <= N_MIX1; ++i) w[i] += (m->x[i] * err + (1 << 15)) >> 16;
}
#endif

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
  int tbits[N_CTX];    /* log2 of the number of blocks per model */
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
  U32 word, prevword, pprevword;
  U8 afterprefix;      /* step 4: the next byte is the 2nd byte of a word code */

  /* step 3 parse state, updated per byte */
  U8 atline, linefirst, col;     /* line start, first byte of the line, table column */
  U8 inlink, linkpart;           /* inside [[...]], 0 target / 1 display text */
  U8 tdepth, tpart;              /* {{...}} depth (capped), 0 name / 1 param / 2 value */
  U8 intag;                      /* inside <...> */
  U8 numlen;                     /* length of the current digit run */
  U32 numhash, numctx;           /* digits so far, the two bytes before the run */
  int kind;                      /* coarse parse state, 0..7 */

  /* match model */
  U32 *mtab;
  U64 mptr;
  int mlen, mbit;
  int mmiss;           /* recent match failures, 0..3 (MSM2) */
  U32 msm[64 * 2];
#if MLONG
  U32 *mtab2;          /* long-context match pointers */
#endif

  Mixer mx;
  Apm a1, a2;
#if MIX2
  Mix1 m1[N_MIX1 - 1];  /* the extra first-layer mixers */
  int m1nsel[N_MIX1 - 1];
  Mix2 m2;
#endif
#if APM_EXT
  Apm a3, a4;
#endif
  int pr_mix, pr;
} Predictor;

static void predictor_init(Predictor *P, U64 cap, U8 *extbuf) {
  memset(P, 0, sizeof *P);
  for (int i = 0; i < N_CTX; ++i) {
#if USE_BITHIST && USE_BUCKETS
    P->tbits[i] = i == 0 ? (TABLE_BITS < 16 ? TABLE_BITS : 16) : i == 1 ? (TABLE_BITS < 22 ? TABLE_BITS : 22) : TABLE_BITS;
    if (i >= 8 && P->tbits[i] > EXTRA_TABLE_BITS) P->tbits[i] = EXTRA_TABLE_BITS;
#else
    P->tbits[i] = TABLE_BITS;
#endif
    /* aligned to 64 bytes so that a 4-block bucket is exactly one cache line */
    U8 *raw = xcalloc(((size_t)16 << P->tbits[i]) * sizeof(Slot) + 64, 1);
    P->table[i] = (Slot *)(raw + ((64 - ((uintptr_t)raw & 63)) & 63));
    P->limit[i] = i < 2 ? 1023 : 255;
#if USE_BITHIST
    for (int s = 0; s < 256; ++s) P->sm[i][s] = s < n_states ? statemap_init(s) : 1u << 31;
#endif
  }
  for (int i = 0; i < 64 * 2; ++i) P->msm[i] = 1u << 31;
  P->cap = cap ? cap : 1;
  /* the compressor can hand over the buffer it already holds: the model only
     ever writes into it the same bytes that are already there */
  P->buf = extbuf ? extbuf : xcalloc(P->cap, 1);
  P->mtab = xcalloc((size_t)1 << MATCH_HASH_BITS, sizeof(U32));
#if MLONG
  P->mtab2 = xcalloc((size_t)1 << MLONG_BITS, sizeof(U32));
#endif
  P->c0 = 1;
  P->atline = 1;
  mixer_init(&P->mx, 256 * 4 * (MIXSEL_PARSE ? 4 : 1));
  apm_init(&P->a1, 256);
  apm_init(&P->a2, 1 << 16);
#if MIX2
  {
    /* weight-set counts of the extra selectors, in MIX2_SETS bit order */
    static const int nsel[4] = {256 << MIX2_O1BITS, 16 * 256, 64 * 8, 8 * 256};
    int j = 0;
    for (int b = 0; b < 4; ++b)
      if (MBIT(b)) { P->m1nsel[j] = nsel[b]; P->m1[j].w = mixer_weights(nsel[b]); ++j; }
  }
  mix2_init(&P->m2, 256);
#endif
#if APM_EXT
  apm_init(&P->a3, 1 << 16);
  apm_init(&P->a4, 33 * 256);
#endif
}

/* choose the 16-counter block for each model; called at each nibble boundary */
static void select_slots(Predictor *P) {
  U32 nib = P->bitpos == 0 ? 0 : P->c0; /* 0 at byte start, 16..31 at mid byte */
  for (int i = 0; i < N_CTX; ++i) {
    U32 h = hash2(P->ctxhash[i], nib + 1);
#if USE_BITHIST
    /* Slot 0 is never used for a bit (positions are 1..15), so it holds an
       8-bit check of the context. A mismatch means another context owns the
       block: start it fresh rather than reuse its histories. */
    U8 check = (U8)(h & 255);
#if USE_BUCKETS
    /* 4-way bucket: take the block whose check matches; otherwise replace the
       one with the least history, judged by the counts in its first slot
       (slot 1 is updated on every visit to the block). Ties go to the lowest
       way, so encoder and decoder always pick the same block. */
    Slot *b = P->table[i] + ((size_t)(h >> (32 - (P->tbits[i] - 2))) << 6);
    int way = -1, victim = 0, best = 1 << 30;
    for (int w = 0; w < 4; ++w) {
      Slot *blk = b + (w << 4);
      if (blk[0] == check) { way = w; break; }
      int pri = st_n0[blk[1]] + st_n1[blk[1]];
      if (pri < best) { best = pri; victim = w; }
    }
    if (way < 0) {
      way = victim;
      memset(b + (way << 4), 0, 16);
      b[way << 4] = check;
    }
    P->slot[i] = b + (way << 4);
#else
    P->slot[i] = P->table[i] + ((size_t)(h >> (32 - P->tbits[i])) << 4);
    if (P->slot[i][0] != check) { memset(P->slot[i], 0, 16); P->slot[i][0] = check; }
#endif
#else
    P->slot[i] = P->table[i] + ((size_t)(h >> (32 - P->tbits[i])) << 4);
#endif
  }
}

static void byte_update(Predictor *P, int c) {
  P->buf[P->pos % P->cap] = (U8)c;
  P->pos++;
  P->c8 = (P->c8 << 8) | (P->c4 >> 24);
  P->c4 = (P->c4 << 8) | (U32)c;

  /* words: letters only, case folded. With the step 4 transform, a word code
     is a whole word and capital flags are invisible to the word model. */
#if USE_PREPROC
  int role = pp_role[c];
  if (P->afterprefix) {
    P->word = hash2(P->word, (U32)c | 0x200);
    P->afterprefix = 0;
  } else if (role == R_CAP || role == R_ALLCAP) {
    /* neither extends nor ends a word */
  } else if (role == R_CODE1) {
    P->word = hash2(P->word, (U32)c | 0x100);
  } else if (role == R_PREFIX) {
    P->word = hash2(P->word, (U32)c | 0x100);
    P->afterprefix = 1;
  } else
#endif
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
    P->word = hash2(P->word, (U32)(c | 32));
  } else if (P->word) {
    P->pprevword = P->prevword;
    P->prevword = P->word;
    P->word = 0;
  }

#if WIKICTX || MIXSEL_PARSE
  /* step 3: wiki parse state. Heuristic, but deterministic: it only looks at
     bytes already coded, so the decoder tracks exactly the same state. */
  int prev = (int)((P->c4 >> 8) & 0xff);
  if (c == 10) { P->atline = 1; P->linefirst = 0; P->col = 0; }
  else if (P->atline) { P->atline = 0; P->linefirst = (U8)c; }
  if (c == '[' && prev == '[') { P->inlink = 1; P->linkpart = 0; }
  else if (c == ']' && prev == ']') P->inlink = 0;
  else if (P->inlink && c == '|') P->linkpart = 1;
  if (c == '{' && prev == '{') { if (P->tdepth < 3) P->tdepth++; P->tpart = 0; }
  else if (c == '}' && prev == '}') { if (P->tdepth) P->tdepth--; P->tpart = 1; }
  else if (P->tdepth && !P->inlink && c == '|') P->tpart = 1;
  else if (P->tdepth && !P->inlink && c == '=' && P->tpart == 1) P->tpart = 2;
  if (c == '<') P->intag = 1;
  else if (c == '>') P->intag = 0;
  if ((c == '|' || c == '!') && (P->linefirst == '|' || P->linefirst == '!') && !P->inlink && !P->tdepth && P->col < 15) P->col++;
  if (c >= '0' && c <= '9') {
    if (!P->numlen) P->numctx = (P->c4 >> 8) & 0xffff;
    if (P->numlen < 15) P->numlen++;
    P->numhash = hash2(P->numhash, (U32)c);
  } else { P->numlen = 0; P->numhash = 0; }
  int lf = P->linefirst;
  P->kind = P->intag ? 5 : P->inlink ? 1 + P->linkpart : P->tdepth ? 3 + (P->tpart != 0)
          : (lf == '|' || lf == '!') ? 6 : (lf == '=' || lf == '*' || lf == '#' || lf == ':') ? 7 : 0;
#endif

  P->ctxhash[0] = hash2(1, P->c4 & 0xff);
  P->ctxhash[1] = hash2(2, P->c4 & 0xffff);
  P->ctxhash[2] = hash2(3, P->c4 & 0xffffff);
  P->ctxhash[3] = hash2(4, P->c4);
  P->ctxhash[4] = hash2(hash2(5, P->c4), P->c8 & 0xffff);
  P->ctxhash[5] = hash2(hash2(6, P->c4), P->c8);
  P->ctxhash[6] = hash2(hash2(7, P->word), P->c4 & 0xff);
  P->ctxhash[7] = hash2(hash2(8, P->word), P->prevword);
#if WIKICTX
  int j = 8;
  U32 kd = (U32)P->kind; (void)kd;
#if WBIT(0)
  P->ctxhash[j++] = hash2(hash2(hash2(9, P->word), kd), P->c4 & 0xff);
#endif
#if WBIT(1)
  P->ctxhash[j++] = hash2(hash2(10, P->c4 & 0xffff), kd);
#endif
#if WBIT(2)
  P->ctxhash[j++] = hash2(hash2(11, (U32)P->col | (U32)P->linefirst << 4 | kd << 12), P->c4 & 0xff);
#endif
#if WBIT(3)
  P->ctxhash[j++] = P->numlen ? hash2(hash2(12, P->numhash), P->numctx | (U32)P->numlen << 16)
                              : hash2(12, 0x1000000 | (P->c4 & 0xffff));
#endif
#if WBIT(4)
  P->ctxhash[j++] = hash2(13, (P->c4 >> 8) & 0xffff);
#endif
#if WBIT(5)
  P->ctxhash[j++] = hash2(hash2(14, P->word), P->pprevword);
#endif
  (void)j;
#endif

  /* match model: extend the current match or look up a new one */
  if (P->mlen > 0 && P->buf[P->mptr % P->cap] == (U8)c) {
    P->mlen++;
    P->mptr++;
#if MSM2
    if ((P->mlen & 31) == 0 && P->mmiss > 0) P->mmiss--;
#endif
  } else {
    P->mlen = 0;
  }
#if MLONG
  /* long table first: a verified long match wins over a short one */
  if (P->pos >= MLONG) {
    U32 h2 = 0;
    const U8 *q = P->buf + (P->pos - MLONG);
    for (int i = 0; i + 4 <= MLONG; i += 4) {
      U32 w; memcpy(&w, q + i, 4);
      h2 = hash2(h2, w);
    }
    h2 >>= 32 - MLONG_BITS;
    if (P->mlen == 0) {
      U64 cand = P->mtab2[h2];
      if (cand > 0) {
        int len = 0;
        while (len < 64 && cand > (U64)len &&
               P->buf[cand - 1 - len] == P->buf[P->pos - 1 - len])
          ++len;
        if (len >= MLONG) { P->mlen = len; P->mptr = cand; }
      }
    }
    P->mtab2[h2] = (U32)P->pos;
  }
#endif
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
#if MATCH_CTX
  /* the byte the match predicts, as context for an ordinary hashed model */
  {
    U32 exp = P->mlen > 0 ? (U32)P->buf[P->mptr % P->cap] : 0x100;
    U32 lb = P->mlen == 0 ? 0 : P->mlen < 16 ? 1 : P->mlen < 32 ? 2 : 3;
    U32 o = MATCH_CTX >= 2 ? (P->c4 & 0xffff) : (P->c4 & 0xff);
    P->ctxhash[N_CTX - 1] = hash2(hash2(15, exp | lb << 9), o);
  }
#endif
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
#if MSM2
      int mi = ((P->mlen < 15 ? P->mlen : 15) * 2 + P->mbit) * 4 + P->mmiss;
#else
      int mi = l * 2 + P->mbit;
#endif
      m->x[N_INPUTS - 2] = stretch(counter_p(P->msm[mi]));
    } else {
      P->mlen = 0;
#if MSM2
      if (P->mmiss < 3) P->mmiss++;
#endif
      m->x[N_INPUTS - 2] = 0;
    }
  } else {
    m->x[N_INPUTS - 2] = 0;
  }
  m->x[N_INPUTS - 1] = 256;

#if MIXSEL_PARSE
  int k4 = P->kind == 0 ? 0 : P->kind <= 2 ? 1 : P->kind <= 4 ? 2 : 3;
  P->pr_mix = mixer_p(m, (int)P->c0 + 256 * (lenb + 4 * k4));
#else
  P->pr_mix = mixer_p(m, (int)P->c0 + 256 * lenb);
#endif
#if MIX2
  {
    int c0 = (int)P->c0, j = 0;
    Mix2 *m2 = &P->m2;
    m2->x[0] = m->dot;
#define MIX1_RUN(S_) do { Mix1 *q = &P->m1[j]; q->sel = (S_) * N_INPUTS; \
      q->dot = mix_dot(m->x, q->w + q->sel); q->pr = squash(q->dot); \
      m2->x[++j] = q->dot; } while (0)
    if (MBIT(0)) MIX1_RUN(c0 | (int)(((P->c4 & 0xff) >> (8 - MIX2_O1BITS)) << 8));
    if (MBIT(1)) MIX1_RUN((P->mlen < 15 ? P->mlen : 15) * 256 + c0);
#if USE_BITHIST
    if (MBIT(2)) {
      int n2 = st_n0[P->st[2]] + st_n1[P->st[2]], n4 = st_n0[P->st[4]] + st_n1[P->st[4]];
#define CBUCKET(n) ((n) == 0 ? 0 : (n) < 2 ? 1 : (n) < 4 ? 2 : (n) < 8 ? 3 : (n) < 16 ? 4 : (n) < 32 ? 5 : (n) < 64 ? 6 : 7)
      MIX1_RUN((CBUCKET(n2) * 8 + CBUCKET(n4)) * 8 + P->bitpos);
    }
#else
    if (MBIT(2)) MIX1_RUN(P->bitpos);
#endif
    if (MBIT(3)) MIX1_RUN(P->kind * 256 + c0);
#undef MIX1_RUN
    m2->x[N_MIX1] = 256;
    P->pr_mix = mix2_p(m2, c0);
  }
#endif
  int p1 = apm_p(&P->a1, P->pr_mix, (int)P->c0);
  int p2 = apm_p(&P->a2, P->pr_mix, (int)(P->c0 | ((P->c4 & 0xff) << 8)));
#if APM_EXT
  int p3 = apm_p(&P->a3, P->pr_mix, (int)(hash2(P->c4 & 0xffff, P->c0) >> 16));
  int ms = P->mbit < 0 ? 0 : 1 + P->mbit + 2 * (P->mlen < 15 ? P->mlen : 15);
  int p4 = apm_p(&P->a4, P->pr_mix, ms * 256 + (int)P->c0);
  int pr = (P->pr_mix * APM_W0 + p1 + p2 + p3 + p4 + (APM_W0 + 4) / 2) / (APM_W0 + 4);
#else
  int pr = (P->pr_mix * 2 + p1 + p2 + 2) >> 2;
#endif
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
#if MSM2
    int mi = ((P->mlen < 15 ? P->mlen : 15) * 2 + P->mbit) * 4 + P->mmiss;
#else
    int l = P->mlen < 63 ? P->mlen : 63;
    int mi = l * 2 + P->mbit;
#endif
    counter_update(&P->msm[mi], y, MSM_LIMIT);
  }
  mixer_update(&P->mx, y);
#if MIX2
  for (int j = 0; j < N_MIX1 - 1; ++j) {
    Mix1 *q = &P->m1[j];
    mix_train(P->mx.x, q->w + q->sel, ((y << 12) - q->pr) * MIXER_LR);
  }
  mix2_update(&P->m2, y);
#endif
  apm_update(&P->a1, y, APM_RATE);
  apm_update(&P->a2, y, APM_RATE);
#if APM_EXT
  apm_update(&P->a3, y, APM_RATE);
  apm_update(&P->a4, y, APM_RATE);
#endif

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

#if USE_PREPROC
/* Archive: 8 bytes input length, 8 bytes length of T, the transform header,
   then the arithmetic-coded T. */
static int compress(const char *in, const char *out) {
  FILE *fi = fopen(in, "rb"), *fo = fopen(out, "wb");
  if (!fi || !fo) { perror("open"); return 1; }
  fseek(fi, 0, SEEK_END);
  U64 n = (U64)ftell(fi);
  fseek(fi, 0, SEEK_SET);
  U8 *a = xcalloc(n ? n : 1, 1);
  if (fread(a, 1, n, fi) != n) { perror("read"); return 1; }
  fclose(fi);

  PPHeader h;
  U64 m = n;
  U8 *t = pp_forward(a, n, &m, &h);
  if (t) free(a); /* keep only T; the model reuses it as its history buffer */
  else t = a;
  fprintf(stderr, "transform: %llu -> %llu bytes, %u dictionary words\n",
          (unsigned long long)n, (unsigned long long)m, h.ndict);

  for (int i = 7; i >= 0; --i) putc_unlocked((int)(n >> (i * 8)) & 255, fo);
  for (int i = 7; i >= 0; --i) putc_unlocked((int)(m >> (i * 8)) & 255, fo);
  pp_header_write(&h, fo);

  static Predictor P;
  predictor_init(&P, m, t);
  select_slots(&P);
  Coder c = {0, 0xffffffff, 0, fo};
#ifdef COST_LOG
  /* with the transform on, costs are per byte of T, not per input byte */
  init_cost();
  char cost_name[4096];
  snprintf(cost_name, sizeof cost_name, "%s.cost", out);
  FILE *fc = fopen(cost_name, "wb");
  if (!fc) { perror("open cost log"); return 1; }
#endif
  for (U64 i = 0; i < m; ++i) {
    int ch = t[i];
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
    progress(i + 1, m);
  }
#ifdef COST_LOG
  fclose(fc);
#endif
  for (int i = 0; i < 4; ++i) { putc_unlocked(c.x1 >> 24, fo); c.x1 <<= 8; }
  fprintf(stderr, "\r%llu -> %ld bytes\n", (unsigned long long)n, ftell(fo));
  fclose(fo);
  return 0;
}

static int file_sink(void *ctx, const U8 *s, size_t n) {
  return fwrite(s, 1, n, (FILE *)ctx) != n;
}

static int decompress(const char *in, const char *out) {
  FILE *fi = fopen(in, "rb"), *fo = fopen(out, "wb");
  if (!fi || !fo) { perror("open"); return 1; }
  U64 n = 0, m = 0;
  for (int i = 0; i < 8; ++i) n = (n << 8) | (U64)getc_unlocked(fi);
  for (int i = 0; i < 8; ++i) m = (m << 8) | (U64)getc_unlocked(fi);
  PPHeader h;
  if (pp_header_read(&h, fi)) { fprintf(stderr, "bad transform header\n"); return 1; }

  static Predictor P;
  predictor_init(&P, m, NULL);
  select_slots(&P);
  Coder c = {0, 0xffffffff, 0, fi};
  for (int i = 0; i < 4; ++i) { int b = getc_unlocked(fi); c.x = (c.x << 8) | (U32)(b == EOF ? 0 : b); }
  for (U64 i = 0; i < m; ++i) {
    for (int b = 0; b < 8; ++b) update(&P, decode_bit(&c, predict(&P)));
    progress(i + 1, m);
  }
  /* the model's history buffer now holds all of T */
  if (pp_inverse(P.buf, m, &h, file_sink, fo)) { fprintf(stderr, "inverse transform failed\n"); return 1; }
  fclose(fi);
  if (ftell(fo) != (long)n) { fprintf(stderr, "length mismatch\n"); return 1; }
  fclose(fo);
  fprintf(stderr, "\rdecompressed %llu bytes\n", (unsigned long long)n);
  return 0;
}
#else
static int compress(const char *in, const char *out) {
  FILE *fi = fopen(in, "rb"), *fo = fopen(out, "wb");
  if (!fi || !fo) { perror("open"); return 1; }
  fseek(fi, 0, SEEK_END);
  U64 n = (U64)ftell(fi);
  fseek(fi, 0, SEEK_SET);
  for (int i = 7; i >= 0; --i) putc_unlocked((int)(n >> (i * 8)) & 255, fo);

  static Predictor P;
  predictor_init(&P, n, NULL);
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
  predictor_init(&P, n, NULL);
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

#endif

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
