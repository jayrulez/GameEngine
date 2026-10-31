#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026-Present Robert Campbell
"""Rewrite stored vector image assets from the UI pipeline's type to VG.Pipeline's (2026-10-22).

The vector image moved out of the UI libraries: `rtti::pipeline::ui::UIVectorImageAsset` became
`rtti::pipeline::vg::VectorImageAsset` (and the cooked `rtti::ui::UIVectorImageSource` became
`rtti::vg::VectorImageSource`). An asset envelope names its type by namespace and name and stores
the type's id in `dataVersions`, so a file written before the move is refused by a build after it.
This script rewrites those three things in each `.xasset` that names the old type. Anything else
is left as it is.

Cooked/ and Sources/ folders are skipped: the first recooks, the second is raw imported data.

Usage:
  scripts/migrate-vector-image.py <path>... [--apply]

<path> is a file or a directory (walked for .xasset). Without --apply it only reports.
"""
import os
import sys

PRIME = 1099511628211
BASIS = 14695981039346656037
MASK = (1 << 64) - 1

OLD = ("rtti::pipeline::ui", "UIVectorImageAsset")
NEW = ("rtti::pipeline::vg", "VectorImageAsset")

#: A project's folders that are not its own stored data.
SKIPPED_DIRECTORIES = {"Cooked", "Sources", ".git"}


def type_id(namespace, name):
    """ComputeTypeId: FNV-1a 64 over "<namespace>::<name>"."""
    h = BASIS
    for byte in (namespace + "::" + name).encode():
        h = ((h ^ byte) * PRIME) & MASK
    return h


def candidate_files(paths):
    for path in paths:
        if os.path.isfile(path):
            yield path
            continue
        for root, directories, names in os.walk(path):
            directories[:] = [d for d in directories if d not in SKIPPED_DIRECTORIES]
            for name in names:
                if name.endswith(".xasset"):
                    yield os.path.join(root, name)


def migrate(text):
    """The migrated text, or None when the file does not name the old type."""
    old_namespace = '<string name="typeNamespace">%s</string>' % OLD[0]
    old_name = '<string name="typeName">%s</string>' % OLD[1]
    if old_namespace not in text or old_name not in text:
        return None
    text = text.replace(old_namespace, '<string name="typeNamespace">%s</string>' % NEW[0])
    text = text.replace(old_name, '<string name="typeName">%s</string>' % NEW[1])
    old_id = '<u64 name="type">%d</u64>' % type_id(*OLD)
    return text.replace(old_id, '<u64 name="type">%d</u64>' % type_id(*NEW))


def main(argv):
    apply = "--apply" in argv
    paths = [a for a in argv[1:] if a != "--apply"]
    if not paths:
        print(__doc__)
        return 2
    changed = 0
    for path in candidate_files(paths):
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        migrated = migrate(text)
        if migrated is None:
            continue
        changed += 1
        print(("migrated " if apply else "would migrate ") + path)
        if apply:
            with open(path, "w", encoding="utf-8", newline="") as handle:
                handle.write(migrated)
    print("%d file(s) %s" % (changed, "migrated" if apply else "to migrate (run with --apply)"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
