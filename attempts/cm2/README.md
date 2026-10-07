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
| `-DCOST_LOG` | Compression also writes `<archive>.cost`: one float32 per input byte, the bits spent on it. The archive is unchanged. Read by [tools/bitcost](../../tools/bitcost) |
| `-DUSE_BITHIST=0` | Step 1 off: plain probability counters, exactly as cm1 (default 1) |
| `-DRUN_INPUT=0` | Step 1 without the per-model run input (default 1) |
| `-DSM_LIMIT=n` | StateMap adaptation limit (default 1023) |
| `-DUSE_BUCKETS=0` | Step 2 off: one block per hash, replaced on any mismatch, exactly as step 1 (default 1) |
| `-DTABLE_BITS=n` | 2^n blocks of 16 bytes per context model. 24 = 256 MB per model, about 1.7 GB in total (default, dev). 26 = 1 GB per model, about 7.2 GB in total on enwik9 (release) |

## Ablation

Every step, measured on enwik8 (10^8 bytes). Archive bytes exclude the binary.

| Step | Change | enwik8 archive bytes | bpc | vs previous | Compress / decompress | Peak RAM |
|---|---|---|---|---|---|---|
| 0 | Baseline: identical to cm1 | 22,075,602 | 1.766 | | 373 s / 326 s (laptop) | 1.9 GB |
| 1 | Bit-history states + StateMaps, run inputs, block checks | 19,706,832 | 1.577 | **-10.7%** | 144 s / 142 s (cloud n2d) | 1.9 GB |
| 2 | 4-way bucketed tables, least-history replacement (dev size) | 19,452,828 | 1.556 | **-1.3%** | 137 s / 139 s (cloud n2d) | 1.8 GB |
| 2 | Same, release size (`TABLE_BITS=26`) | 19,406,054 | 1.552 | -1.5% | 148 s / 149 s (cloud n2d) | 6.3 GB |

Times come from different machines until a cloud baseline exists: the cloud
n2d core is about twice as fast as the laptop. On the same machine, step 1
costs about 15% more time (enwik7: 32 s to 37 s compressing on the laptop).
The bpc column is archive only; the leaderboard adds the binary.

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
