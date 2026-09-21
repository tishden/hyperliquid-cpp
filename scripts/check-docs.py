#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Denis Tishkov <denis8825@ya.ru>
"""Check every relative link and heading anchor in the Markdown documentation.

Catches the two ways documentation rots as it is edited: a file that was renamed or removed, and
a section that was renumbered while the links to it were not. Both have happened here.

    scripts/check-docs.py        # exits non-zero if anything is broken
"""
import glob
import html
import os
import re
import sys

# GitHub builds an anchor by lowercasing the heading, dropping punctuation and joining words with
# hyphens. Letters outside ASCII (µ, Greek, Cyrillic) survive, so they must not be stripped here.
_PUNCT = re.compile(r"[^\w \-µͰ-ϿЀ-ӿ]")


def slug(heading: str) -> str:
    text = html.unescape(heading).lower()
    text = re.sub(r"`|\*", "", text)
    return _PUNCT.sub("", text).strip().replace(" ", "-")


def main() -> int:
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    os.chdir(root)
    files = sorted(glob.glob("docs/*.md")) + ["README.md", "CHANGELOG.md", "THIRD_PARTY_NOTICES.md",
                                           "CONTRIBUTING.md", "SECURITY.md"]
    files = [f for f in files if os.path.exists(f)]

    anchors = {}
    for f in files:
        headings = re.findall(r"^#+\s+(.*)$", open(f, encoding="utf-8").read(), re.M)
        anchors[os.path.normpath(f)] = {slug(h) for h in headings}

    broken = []
    for f in files:
        for m in re.finditer(r"\[[^\]]+\]\(([^)#]*)(#[^)]*)?\)", open(f, encoding="utf-8").read()):
            target, anchor = m.group(1), m.group(2)
            if target.startswith(("http://", "https://", "mailto:")) or " " in target or "::" in target:
                continue
            path = os.path.normpath(os.path.join(os.path.dirname(f), target)) if target else os.path.normpath(f)
            if target and not os.path.exists(path):
                broken.append(f"{f}: missing file → {target}")
            elif anchor and path in anchors and anchor[1:] not in anchors[path]:
                broken.append(f"{f}: missing anchor → {target}{anchor}")

    for line in broken:
        print(f"  {line}")
    print(f"check-docs: {len(files)} files, {'OK' if not broken else str(len(broken)) + ' broken link(s)'}")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
