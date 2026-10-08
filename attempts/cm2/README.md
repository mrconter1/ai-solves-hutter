# cm2

The second attempt. It starts as an exact copy of [cm1](../cm1): same model,
same output. Each step from the plan is added behind a compile-time flag,
measured on enwik8, and kept only if it gains. The full enwik9 run happens
once, when all steps are in.

```
cm2 c <input> <archive>
cm2 d <archive> <output>
```

Build flags:

| Flag | Effect |
|---|---|
| `-DCOST_LOG` | Compression also writes `<archive>.cost`: one float32 per coded byte, the bits spent on it. The archive is unchanged. Read by [tools/bitcost](../../tools/bitcost); lines up with the input only with `-DUSE_PREPROC=0` |
| `-DUSE_BITHIST=0` | Step 1 off: plain probability counters, exactly as cm1 (default 1) |
| `-DRUN_INPUT=0` | Step 1 without the per-model run input (default 1) |
| `-DSM_LIMIT=n` | StateMap adaptation limit (default 1023) |
| `-DUSE_BUCKETS=0` | Step 2 off: one block per hash, replaced on any mismatch, exactly as step 1 (default 1) |
| `-DTABLE_BITS=n` | 2^n blocks of 16 bytes per context model. 24 = 256 MB per model, about 1.7 GB in total (default, dev). 26 = 1 GB per model, about 7.2 GB in total on enwik9 (release) |
| `-DWIKICTX=mask` | Step 3 models: 1 word + parse state, 2 order-2 + parse state, 4 line/table column, 8 number, 16 sparse (bytes 2-3 back), 32 word + word before previous. Default 37 (1+4+32); 0 = step 2 exactly |
| `-DMIXSEL_PARSE=0` | Step 3 off for the mixer: weight set chosen without the parse state (default 1) |
| `-DEXTRA_TABLE_BITS=n` | Table size cap for the step 3 models (default 23, 128 MB each) |
| `-DUSE_PREPROC=0` | Step 4 off: no transform, old archive format, exactly as step 3 (default 1) |
| `-DPP_CAPS=0`, `-DPP_ENT=0`, `-DPP_DICT=0` | Step 4 parts off one by one: capital flags, entity bytes, word dictionary (default all 1) |
| `-DPP_K1=n` | Words with a one-byte code (default 32) |
| `-DPP_MINLEN=n`, `-DPP_MINCOUNT=n` | Shortest word and fewest occurrences worth a code (default 2 and 8) |
| `-DPP_SORT=n` | Order of the 2-byte codes: 0 by frequency, 1 alphabetical (default), 2 by suffix |
| `-DMIX2=0` | Step 5 off for mixing: one mixer, exactly as step 4 (default 1) |
| `-DMIX2_SETS=mask` | Extra first-layer mixers by weight-set selector: 1 order-1 byte, 2 match length, 4 order-3/6 confidence, 8 parse kind. Default 4 |
| `-DMIX2_O1BITS=n` | High bits of the previous byte used by the order-1 selector (default 8) |
| `-DMIX2_LR=n` | Second-layer mixer learning rate (default 2) |
| `-DAPM_EXT=0` | Step 5 off for the final stage: only the two step 1 APMs (default 1) |
| `-DAPM_W0=n` | Weight of the mixer output against each APM in the final average (default 0, APMs only) |

## Ablation

Every step, measured on enwik8 (10^8 bytes). Archive bytes exclude the binary.

| Step | Change | enwik8 archive bytes | bpc | vs previous | Compress / decompress | Peak RAM |
|---|---|---|---|---|---|---|
| 0 | Baseline: identical to cm1 | 22,075,602 | 1.766 | | 373 s / 326 s (laptop) | 1.9 GB |
| 1 | Bit-history states + StateMaps, run inputs, block checks | 19,706,832 | 1.577 | **-10.7%** | 144 s / 142 s (cloud n2d) | 1.9 GB |
| 2 | 4-way bucketed tables, least-history replacement (dev size) | 19,452,828 | 1.556 | **-1.3%** | 137 s / 139 s (cloud n2d) | 1.8 GB |
| 2 | Same, release size (`TABLE_BITS=26`) | 19,406,054 | 1.552 | -1.5% | 148 s / 149 s (cloud n2d) | 6.3 GB |
| 3 | Wiki parse state: 3 structure models + parse-based mixer selection (`WIKICTX=37`, dev size) | 19,056,553 | 1.525 | **-2.0%** | 215 s / 212 s (cloud n2d) | 2.1 GB |
| 3 | Variant: all 5 structure models (`WIKICTX=55`), not kept | 19,008,680 | 1.521 | -2.3% | 255 s / 255 s (cloud n2d) | 2.4 GB |
| 4 | Reversible transform: capital flags, entity bytes, word dictionary (32 one-byte codes) | 18,602,720 | 1.488 | **-2.4%** | 178 s / 170 s (cloud n2d) | 2.1 GB |
| 4 | Variant: 40 one-byte codes (`PP_K1=40`), not kept | 18,664,637 | 1.493 | -2.1% | 181 s / 183 s (cloud n2d) | 2.1 GB |
| 5 | 2-layer mixing (extra mixer chosen by order-3/6 confidence) + order-2 and match APMs, APMs only in the final average | 18,443,050 | 1.475 | **-0.86%** | 200 s / 196 s (cloud n2d) | 2.1 GB |
| 5 | Variant: also an order-1 selector (`MIX2_SETS=5`), not kept | 18,358,749 | 1.469 | -1.31% | 238 s / 233 s (cloud n2d) | 2.1 GB |

Times come from different machines until a cloud baseline exists: the cloud
n2d core is about twice as fast as the laptop. On the same machine, step 1
costs about 15% more time (enwik7: 32 s to 37 s compressing on the laptop).
The bpc column is archive only; the leaderboard adds the binary.

## Step 5: two-layer mixing and more APMs

The main mixer (weight set chosen by the partial byte, match length and parse
state, from step 3) is now one of several first-layer mixers. They all see
the same inputs, but each picks its weight set by a different context and
learns from its own error. A small second-layer mixer, with a weight set per
partial byte, combines their outputs. The final stage gets two more APMs, one
on a hashed order-2 context and one on the match state (expected bit and
length), and the final probability is now the plain average of the four APMs
(`APM_W0=0`), without the raw mixer output.

Shipped: one extra first-layer mixer, chosen by the confidence of the order-3
and order-6 models (their bit-history counts, bucketed, times the bit
position). Everything is behind `MIX2` and `APM_EXT`; `MIX2=0 APM_EXT=0`
reproduces step 4 exactly (enwik7 2,046,119).

**Per idea on enwik7** (step 4: 2,046,119; laptop runs before WSL was retired,
times are noisy):

| Variant | enwik7 bytes | vs step 4 |
|---|---|---|
| Extra APMs only, mixer weight 4 | 2,044,624 | -0.07% |
| Extra APMs only, APMs only (`APM_W0=0`, cloud) | 2,037,339 | -0.43% |
| All four extra selectors, no extra APMs | 2,029,937 | -0.79% |
| Order-1 selector alone | 2,033,791 | -0.60% |
| Match-length selector alone | 2,043,284 | -0.14% |
| Confidence selector alone | 2,038,554 | -0.37% |
| Parse-kind selector alone | 2,041,797 | -0.21% |
| Second-layer rate 1 / 4 (all selectors) | 2,030,539 / 2,029,703 | -0.76% / -0.80% |
| Order-1 + confidence, APMs, mixer weight 4 / 2 / 1 / 0 | 2,029,168 / 2,027,109 / 2,025,844 / 2,024,596 | -0.83% / -0.93% / -0.99% / -1.05% |
| **Confidence selector + APMs, weight 0 (shipped)** | **2,030,533** | **-0.76%** |
| Order-1 + confidence + kind, weight 0 | 2,023,177 | -1.12% |
| All four selectors, weight 0 | 2,022,900 | -1.13% |

**enwik8 candidates**, one cloud batch VM (five variants side by side, so the
times are inflated by contention and only comparable to each other):

| Variant | enwik8 bytes | vs step 4 | Decompress time vs step 4 in the batch |
|---|---|---|---|
| Confidence selector (shipped) | 18,443,050 | -0.86% | +38% |
| Order-1 + confidence | 18,358,749 | -1.31% | +57% |
| Same, order-1 from the top 5 bits only | 18,397,985 | -1.10% | +55% |
| Order-1 + confidence + kind | 18,354,105 | -1.34% | +72% |

On a dedicated core the shipped variant costs only +13% / +15% (200 s / 196 s
against 178 s / 170 s), and order-1 + confidence +34% / +37%. The order-1
selector (65,536 weight sets) is the expensive one: its extra -0.46% isn't worth
about +20% time, so it stays off; it's the first thing to switch on for a
release if time allows. Fewer order-1 bits don't save time, because the cost
is the extra mixer, not the size of its table.

Edge cases: all 11 pass on the shipped default (run in the cloud through
`bench/cloud/gcp-batch.sh`).

## Step 4: reversible text transform

`preproc.c` (included by `cm2.c`, so cm2 is still one program) rewrites the
input before the model sees it, and the decompressor runs the inverse after
decoding:

- **Capital flags.** `The` becomes a CAP flag plus `the`, `THE` an ALLCAP
  flag plus `the`. All forms of a word then share statistics. Mixed-case words
  (`McDonald`, `iPhone`) stay as they are.
- **Entity bytes.** `&amp;` `&quot;` `&lt;` `&gt;` become one byte each.
- **Word dictionary.** The compressor counts the words in the input and gives
  codes to the most valuable ones (occurrences x length): the top 32 get a
  one-byte code, the rest a two-byte code (a prefix byte plus a byte in
  0x80..0xff, sorted alphabetically). The dictionary is sent at the start of
  the coded stream, one word per line, so the model compresses it like text.

Every flag, entity byte and code is a byte value that never occurs in the
input (enwik9 has 50 of them), so nothing needs escaping. A 38-byte raw header
lists the free byte values; both sides derive the same roles from it. When too
few are free (binary data) the transform switches itself off. The compressor
also runs the inverse over the transformed stream and compares it with the
input before using it, so a transform bug falls back to "off" instead of a
broken archive. The word model treats a code as a whole word and ignores the
capital flags; the step 3 parse state is unaffected, because codes never look
like ASCII punctuation.

On enwik8 the transform shortens the coded stream by about a quarter
(100,000,000 to 76,419,491 bytes), which is also why it is 17% faster than
step 3. The dictionary there has 1,696 words, which is all the code space the
free byte values allow (13 prefix bytes x 128 + 32). A bigger dictionary would
need more code space, for example 3-byte codes; that is left for later. Memory: the compressor briefly holds the
input plus the transformed stream plus a 128 MB word table, then frees the
input before the model starts, and the model reuses the transformed stream as
its history buffer, so peak RAM does not grow.

**Per idea on enwik7** (step 3: 2,070,142 bytes; times are laptop compress):

| Variant | enwik7 bytes | Change | Note |
|---|---|---|---|
| Capital flags only | 2,071,037 | +0.04% | alone it does not pay |
| Entity bytes only | 2,068,270 | -0.09% | |
| Capitals + entities | 2,069,119 | -0.05% | |
| Dictionary only, no capital flags | 2,079,867 | +0.47% | codes miss every capitalised word |
| All three, 2-byte codes only, alphabetical | 2,067,661 | -0.12% | |
| Same, codes by frequency | 2,078,036 | +0.38% | |
| Same, codes by suffix | 2,074,805 | +0.23% | |
| 16 one-byte codes | 2,059,150 | -0.53% | |
| 32 one-byte codes | 2,055,696 | -0.70% | |
| 40 one-byte codes | 2,055,402 | -0.71% | |
| 32 one-byte codes, words of 2+ letters | **2,046,119** | **-1.16%** | **kept** |
| 28 / 36 / 40 one-byte codes, 2+ letters | 2,046,411 / 2,045,208 / 2,045,321 | -1.15% to -1.20% | a plateau |
| 32 one-byte codes, at most 1000 words | 2,055,839 | -0.69% | |
| words of 4+ / 5+ letters only | 2,075,251 / 2,082,769 | +0.25% / +0.61% | |

What mattered: one-byte codes for the most frequent short words (`the`, `of`,
`and`, `in`), which only pay together with capital flags. The two-byte codes
add little on their own. Without the flags the dictionary misses every
capitalised word and loses. On enwik8 the gain doubles to -2.4%, and 32 beats
40 one-byte codes there, so 32 is the default.

The binary grows from 50.6 KB to 58.8 KB. Total (archive + binary) on enwik8
goes from 19,107,129 to 18,661,488 bytes (-2.33%).

`tools/edgecases.sh` has four new cases for the transform: mixed case forms and
broken entities next to UTF-8 letters, words longer than 255 letters, input
containing every byte value (the transform must switch off) and input using
most control bytes (few free values left). All 11 cases round-trip, and the
transform is confirmed active on the text cases.

## Step 3: wiki structure contexts

Step 0 showed that about 92% of the cost is natural language, in article text,
links and templates alike, while XML is already cheap. So step 3 tracks where
in the wiki markup the coder is, and lets a few models and the mixer use it.

A **parse state** is updated after every byte, from bytes already coded, so
the decoder follows exactly the same state:

- inside a `[[link]]`, and whether in the target or the display text after `|`
- inside a `{{template}}` (depth capped at 3), and whether in the name, a
  parameter or a value after `=`
- inside an XML tag
- the first byte of the line (table row, heading, list) and the table column
- digit runs, for a number model

From that, a coarse **kind** (text, link target, link text, template name,
template parameter, tag, table line, heading or list line) feeds the new models,
and four coarse groups of it (text, link, template, other) are added to the
mixer's weight-set selector.

Each candidate on enwik7, against step 2 (2,113,216 bytes, 16.6 s user time
on the laptop, one core):

| Variant | enwik7 bytes | vs step 2 | Time |
|---|---|---|---|
| Mixer selection by parse state only | 2,096,387 | -0.80% | +8% |
| + word and parse state (1) | 2,093,624 | -0.93% | |
| + order-2 and parse state (2) | 2,094,274 | -0.90% | |
| + line type and table column (4) | 2,093,440 | -0.94% | |
| + number (8) | 2,113,460 | +0.01% | |
| + sparse, bytes 2-3 back (16) | 2,109,805 | -0.16% | |
| + word and the word before the previous one (32) | 2,106,287 | -0.33% | |
| 1+4, with mixer selection | 2,075,599 | -1.78% | +15% |
| **1+4+32, with mixer selection (shipped)** | **2,070,142** | **-2.04%** | **+20%** |
| 1+2+4, with mixer selection | 2,072,593 | -1.92% | +51% |
| 1+2+4+32, with mixer selection | 2,067,156 | -2.18% | +62% |
| 1+2+4+16+32, with mixer selection | 2,065,191 | -2.27% | +103% |
| all six, with mixer selection | 2,066,968 | -2.19% | |

(The single-model rows were measured without mixer selection, and their times
were not taken on a quiet machine, so they are left blank.)

What did not help: the **number model** gained nothing, even though numbers cost
3.8 bpc in step 0; they are mostly years, dates and IDs that the order-n and
match models already handle as well as this context can. The **sparse model**
and the **order-2 + parse state** model each gained a little but cost a lot of
time, so they are left out of the default.

On enwik8 the shipped set saves 2.0% (19,452,828 to 19,056,553). Its time
cost on the cloud n2d core is larger than on the laptop: 215 s against 137 s
per direction, +57%. That is still far inside the budget (cm2 on enwik9
should take well under an hour per direction, against the 50 h limit), so
compression wins here. `WIKICTX=5` is the cheaper fallback (-1.8% on enwik7).

The three new models use tables capped at 128 MB each (`EXTRA_TABLE_BITS=23`),
so the release configuration grows by about 0.4 GB, to roughly 7.6 GB on enwik9.

## Step 2: bucketed hash tables

Step 1 replaced a block whenever another context hashed onto it. Step 2 groups
blocks into **4-way buckets** of 64 bytes, exactly one cache line. A context
looks for its check byte among the 4 blocks of its bucket. On a miss it takes
over the block with the **least history**, judged by the counts in that
block's first slot (slot 1 is updated on every visit). Ties go to the lowest
way, so the encoder and decoder always pick the same block.

Table size is now a build flag (`TABLE_BITS`). The order-1 and order-2 models
have few contexts and get small tables (1 MB and 64 MB), which frees memory
for the higher orders.

| enwik7 test | Step 1 | Step 2 |
|---|---|---|
| Default tables (256 MB per model) | 2,117,804 | 2,113,216 (-0.2%) |
| Squeezed tables (`TABLE_BITS=19`, 8 MB per model) | 2,194,838 | 2,140,420 (-2.5%) |

The squeezed test shows what buckets are for: they matter once the tables are
full, which is the normal state on enwik9 (1 GB of input into 1.7 GB of
tables). On enwik8 the gain is 1.3% at the dev size and 1.5% at the release
size, so most of step 2's value should show up on enwik9.

Release runs use `CFLAGS="-DTABLE_BITS=26"`, passed through the runner
(`CFLAGS=... bench/cloud/gcp-run.sh cm2 enwik9`) and recorded in the CSV note.
On enwik8 that peaks at 6.3 GB; on enwik9 the input buffer adds 0.9 GB, about
7.2 GB in total, under the 10 GB cap.

## Step 1: bit-history states

cm1 kept one 32-bit probability counter per context slot. cm2 keeps a one-byte
**bit-history state** instead: bounded counts of the zeros and ones recently
seen in that context, plus the last bit when both counts are non-zero. On each
bit the matching count goes up and a large opposite count is cut to about
half, so recent behaviour dominates. The smaller count may reach 6 and the
larger one is capped lower as the smaller grows, giving 237 states. The
transition table is generated at startup.

A **StateMap** per model learns what probability each state really stands for,
from experience rather than a formula. Each model feeds the mixer two inputs:
the StateMap's stretched probability, and a **run input** that is non-zero only
when the context has seen one bit value so far, larger for longer runs.

With one-byte slots a 16-slot block is 16 bytes, so each 256 MB table holds 4x
as many blocks as in cm1. Slot 0 of each block is never used for a bit, so it
stores an **8-bit check** of the context; when a different context hashes to
the same block, the block starts fresh instead of reusing foreign histories.
(That is part of step 2's idea; it came for free with the new layout.)

Variants tried on enwik7 (cm1: 2,449,272 bytes):

| Variant | enwik7 archive bytes |
|---|---|
| Step 1 as shipped | **2,117,804** (-13.5%) |
| Without the run input | 2,125,351 |
| StateMap limit 255 | 2,117,844 |
| StateMap limit 127 | 2,119,461 |

`tools/edgecases.sh cm2` round-trips an empty file, 1 byte, 64 KB of random
bytes, 1 MB of zeros, 400 KB without newlines, high bytes and wiki-like text.
All pass.

## Step 0: where the bits go

cm1's cost on enwik8, split by region type with
[tools/bitcost](../../tools/bitcost). Classes are exclusive: a number inside a
link counts as link, a link inside a template counts as template, and every
whole metadata line (`<title>`, `<id>`, `<timestamp>`, `<username>`, ...)
counts as xml.

| Class | Bytes | % of input | Cost (bytes) | bpc | % of cost |
|---|---|---|---|---|---|
| text | 59,212,657 | 59.2% | 13,454,034 | 1.818 | 61.0% |
| link | 15,807,663 | 15.8% | 3,633,737 | 1.839 | 16.5% |
| template | 13,897,595 | 13.9% | 3,206,297 | 1.846 | 14.5% |
| table | 3,058,558 | 3.1% | 560,163 | 1.465 | 2.5% |
| number | 864,771 | 0.9% | 408,394 | 3.778 | 1.9% |
| xml | 4,396,371 | 4.4% | 373,455 | 0.680 | 1.7% |
| heading | 1,109,900 | 1.1% | 234,729 | 1.692 | 1.1% |
| entity | 1,652,485 | 1.7% | 179,646 | 0.870 | 0.8% |

The most expensive 1 KB windows:

| Offset | bpc | Starts with |
|---|---|---|
| 0 | 7.142 | `<mediawiki xmlns="http://www.mediawiki.org/xml/export-0.3/" ` |
| 4,096 | 5.605 | `e="preserve">{{Anarchism}} '''Anarchism''' originated as a t` |
| 201,728 | 4.526 | `eatest and the most [[central character]] of [[Homer]]'s ''[` |
| 9,655,296 | 4.334 | `.....:;tZ0SKbE@#MMMMMMMMC. .,;tCC7C%C%%t; ,.,,,,:;;:..,NMMMM` |
| 62,464 | 4.239 | `http://www.iww.org/ Industrial Workers of the World]  &lt;!-` |
| 67,286,016 | 4.186 | ` \| suoni \| soon \| suotna, suona \| suona \| šön \| sən \| jan` |
| 32,159,744 | 4.184 | `Guǎngdōng]] (广东) *[[Guizhou\|Guìzhōu]] (贵州) *[[Ha` |
| 6,144 | 4.183 | `archists also offer positive visions of what they believe to` |
| 64,028,672 | 4.121 | `MN'''  :Karjatades kundikarju, :Süües musti hooramarju, :L` |
| 5,120 | 4.098 | `67;&amp;#943;&amp;#945;]]'' (&quot;without [[archon]]s (rule` |

Conclusions:

- **About 92% of the cost is natural language:** article text (61%), links
  (16.5%) and templates (14.5%). Links and templates cost the same per byte as
  plain text (about 1.8 bpc), because their content is mostly words. Better
  general text modelling (steps 1, 4 and 5) is where the size is.
- **Markup is already cheap.** XML metadata costs 0.68 bpc and 1.7% of the
  total, tables 2.5%, headings 1.1%, entities 0.8%. Wiki-aware contexts (step 3)
  should therefore target the structure inside links and templates (link
  target vs display text, template names and parameter keys), not the XML.
- **Numbers are the most expensive bytes per byte**, at 3.8 bpc, but only 1.9% of
  the cost. A dedicated number context could save around 1%.
- **The worst windows are the start of the file**, where the model is still
  cold, plus foreign-language text, ASCII art, base64 and chess notation.
  They're real but small. The model warms up within the first ~10 KB.
- **Priority for the next steps:** 1 (bit histories) and 4 (capitalization and
  a word dictionary) first, then 5 (mixing), then a narrow step 3 aimed at
  links, templates and numbers.
