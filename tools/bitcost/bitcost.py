#!/usr/bin/env python3
"""Where do the bits go? Splits a compressor's per-byte cost by region type.

    python3 bitcost.py <input> <cost-file> [--windows N]

<cost-file> holds one float32 per input byte: the bits the compressor spent on
that byte (cm2 writes it when built with -DCOST_LOG). Every byte of the input
gets exactly one class. Classes are assigned in increasing priority, so a later
rule overrides an earlier one: a number inside a link counts as link, a link
inside a template counts as template.
"""
import re
import sys

import numpy as np

# (name, regex, flags), lowest priority first. Applied to the raw bytes.
RULES = [
    ("number", rb"[0-9]+", 0),
    ("heading", rb"^=+[^\n]*?=+ *$", re.M),
    ("link", rb"\[\[[^\[\]\n]*\]\]", 0),
    ("entity", rb"&(?:[a-zA-Z]+|#[0-9]+);", 0),
    ("table", rb"^(?:\{\||\|\}|\||!)[^\n]*$", re.M),
    # whole metadata lines: <title>, <id>, <timestamp>, <username>, ... but not <text ...> lines
    ("xml", rb"^ *<(?!text)[^\n]*$", re.M),
    ("xml", rb"<text[^>\n]*>|</text>", 0),
]
TEMPLATE = re.compile(rb"\{\{(?:(?!\{\{|\}\})[\s\S])*?\}\}")
CLASSES = ["text", "number", "heading", "link", "entity", "table", "template", "xml"]


def classify(data):
    labels = np.zeros(len(data), dtype=np.uint8)  # 0 = plain article text
    for name, pattern, flags in RULES:
        if name == "table":
            mark_templates(data, labels)  # templates sit between links/entities and tables
        k = CLASSES.index(name)
        for m in re.finditer(pattern, data, flags):
            labels[m.start():m.end()] = k
    return labels


def mark_templates(data, labels):
    """{{...}} nests, so mark innermost templates first and blank them out."""
    k = CLASSES.index("template")
    work = bytearray(data)
    for _ in range(6):
        found = False
        for m in TEMPLATE.finditer(work):
            labels[m.start():m.end()] = k
            work[m.start():m.end()] = b"x" * (m.end() - m.start())
            found = True
        if not found:
            break


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    nwin = 20
    if "--windows" in sys.argv:
        nwin = int(sys.argv[sys.argv.index("--windows") + 1])
    data = open(sys.argv[1], "rb").read()
    cost = np.fromfile(sys.argv[2], dtype=np.float32).astype(np.float64)
    if len(cost) != len(data):
        sys.exit(f"cost file has {len(cost)} entries, input has {len(data)} bytes")

    labels = classify(data)
    nbytes = np.bincount(labels, minlength=len(CLASSES))
    bits = np.bincount(labels, weights=cost, minlength=len(CLASSES))
    total_bits = cost.sum()

    print(f"input {len(data):,} bytes, cost {total_bits / 8:,.0f} bytes ({total_bits / len(data):.4f} bpc)\n")
    print("| Class | Bytes | % of input | Cost (bytes) | bpc | % of cost |")
    print("|---|---|---|---|---|---|")
    for k in np.argsort(-bits):
        if nbytes[k] == 0:
            continue
        print(f"| {CLASSES[k]} | {nbytes[k]:,} | {100 * nbytes[k] / len(data):.1f}% | "
              f"{bits[k] / 8:,.0f} | {bits[k] / nbytes[k]:.3f} | {100 * bits[k] / total_bits:.1f}% |")

    if nwin:
        w = 1024
        m = len(cost) // w
        win = cost[: m * w].reshape(m, w).sum(axis=1)
        print(f"\nMost expensive {w}-byte windows:\n")
        print("| Offset | bpc | Starts with |")
        print("|---|---|---|")
        for i in np.argsort(-win)[:nwin]:
            snippet = data[i * w : i * w + 60].decode("utf-8", "replace")
            snippet = snippet.replace("\n", " ").replace("|", "\\|")
            print(f"| {i * w:,} | {win[i] / w:.3f} | `{snippet}` |")


if __name__ == "__main__":
    main()
