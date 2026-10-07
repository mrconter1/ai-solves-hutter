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

## Ablation

Every step, measured on enwik8 (10^8 bytes). Archive bytes exclude the binary.

| Step | Change | enwik8 archive bytes | bpc | vs previous |
|---|---|---|---|---|
| 0 | Baseline: identical to cm1 | 22,075,602 | 1.766 | |

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
