# cm1

The first baseline: a small context-mixing compressor in about 400 lines of plain C.
It follows the PAQ/lpaq recipe, stripped down to the parts that matter most.

```
cm1 c <input> <archive>
cm1 d <archive> <output>
```

## Model

| Component | What it does |
|---|---|
| Order-1, 2, 3, 4, 6, 8 contexts | Hashed byte histories. Each predicts the next bit from what followed this context before |
| Word context | Hash of the current word (letters only, case folded) plus the previous byte |
| Word bigram | The current word and the previous word |
| Match model | Finds the last occurrence of the previous 7 bytes and predicts that what followed it then comes again. Its confidence is learned per match length |
| Mixer | Logistic mixing of all predictions. Weight sets are chosen by the partial byte and a match-length bucket |
| 2 APMs | Refine the mixed probability by the partial byte, and by the partial byte plus the previous byte |
| Arithmetic coder | 32-bit binary coder with 12-bit probabilities |

The counters are lpaq-style: a 22-bit probability plus a 10-bit count, with
learning rate 1/(n+1.5). The hash tables are split into 64-byte blocks of 16
counters, one block per nibble, so each model costs about two cache misses per
byte. There is no collision detection.

Memory: 8 x 256 MB tables, plus the input buffer for the match model (1 GB on
enwik9), plus 64 MB of match pointers. That's about 3.2 GB on enwik9.

## Tuning notes

- Mixer learning rate: swept on enwik7. Going from `err*12 >> 14` (the first
  logged row, 2.083 bpc) to `err*4 >> 16` gave 1.959 bpc. Values of 2 to 6
  were within 0.1% of each other.

## Obvious next steps

1. Bit-history state machines instead of plain probability counters. This is the biggest single gap to lpaq.
2. Checksums in the hash blocks to cut collision damage.
3. An enwik-specific preprocessing pass: a word dictionary transform and XML/markup handling.
4. More mixer inputs per model (confidence, run information) and a second mixer layer.
5. An online LSTM or small transformer as an extra model (the cmix/nncp direction).
