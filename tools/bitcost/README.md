# bitcost

Shows where a compressor spends its bits, split by the kind of content in
enwik: article text, links, templates, tables, numbers, XML metadata and so on.

```bash
# 1. build a compressor that logs per-byte cost (cm2 has a flag for it)
CFLAGS=-DCOST_LOG bash attempts/cm2/build.sh
attempts/cm2/bin/cm2 c data/enwik8 work/enwik8.cmp      # also writes work/enwik8.cmp.cost
bash attempts/cm2/build.sh                              # rebuild without the flag

# 2. split the cost by region type (needs python3 + numpy)
python3 tools/bitcost/bitcost.py data/enwik8 work/enwik8.cmp.cost [--windows 20]
```

The cost file is one float32 per input byte: the bits spent coding that byte,
so it is 4x the input size (400 MB for enwik8). The output is a markdown table
of bytes, cost and bpc per class, plus the most expensive 1 KB windows.

Every byte gets exactly one class, assigned in increasing priority: number,
heading, link, entity, template, table, xml. A later class overrides an earlier
one, so a number inside a link counts as link. The rules are regexes over the
raw bytes, at the top of `bitcost.py`. They are approximations (for example,
a link that spans a line break is not recognised), which is good enough for
deciding where to spend effort.

Takes about a minute on enwik8.
