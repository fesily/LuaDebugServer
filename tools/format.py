#!/usr/bin/env python3
"""Run clang-format over the project's C/C++ sources.

Replaces the old `compile/clang-format.lua` (which needed luamake's
`clang-format` wrapper and bee's filesystem bindings). The formatter version is
pinned: `.clang-format` was tuned against this major, and clang-format's output
changes between majors, so a different one produces diffs that are not style
choices. A binary of another major is refused instead of silently reformatting
the tree with it (pass `--formatter` to override deliberately).

The CI job runs exactly this script, so `--check` locally and the workflow agree
byte for byte and the version lives in one place.

Usage:
  tools/format.py            # rewrite files in place
  tools/format.py --check    # exit non-zero when a file would change
"""

import argparse
import os
import shutil
import subprocess
import sys

EXTENSIONS = (".h", ".inl", ".c", ".cpp")
SOURCE_ROOT = "src"
# Keep in sync with the CI job (`.github/workflows/clang-format.yml`), which
# installs the PyPI wheel of this major: the newest LLVM release, and the same
# binary on every runner and developer machine regardless of the distro's clang.
PINNED_MAJOR = "23"
CANDIDATE_FORMATTERS = ("clang-format-" + PINNED_MAJOR, "clang-format")


def formatter_major(path):
    """Major version clang-format reports, or None when it cannot be read."""
    try:
        out = subprocess.run([path, "--version"], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    text = out.stdout or ""
    # "Ubuntu clang-format version 22.1.2 (1ubuntu1)"
    for token in text.split():
        head = token.split(".")[0]
        if head.isdigit():
            return head
    return None


def find_formatter(explicit):
    if explicit:
        return explicit
    for name in CANDIDATE_FORMATTERS:
        path = shutil.which(name)
        if path:
            return path
    return None


def collect_sources(root):
    sources = []
    for directory, _, files in os.walk(root):
        for name in sorted(files):
            if name.endswith(EXTENSIONS):
                sources.append(os.path.join(directory, name))
    return sources


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="do not modify files")
    parser.add_argument("--formatter", help="explicit clang-format binary")
    args = parser.parse_args()

    formatter = find_formatter(args.formatter)
    if not formatter:
        print("clang-format not found (tried %s)" % ", ".join(CANDIDATE_FORMATTERS),
              file=sys.stderr)
        return 2
    major = formatter_major(formatter)
    if major != PINNED_MAJOR and not args.formatter:
        # Reformatting with another major would rewrite the whole tree into a
        # different style and make the next CI run disagree; refusing is the only
        # outcome that keeps the two in sync.
        print("%s reports clang-format major %s, but this tree is formatted with %s "
              "(%s). Install clang-format-%s, or pass --formatter explicitly."
              % (formatter, major or "unknown", PINNED_MAJOR, formatter, PINNED_MAJOR),
              file=sys.stderr)
        return 2

    sources = collect_sources(SOURCE_ROOT)
    if not sources:
        return 0

    if args.check:
        command = [formatter, "--style=file", "--dry-run", "--Werror", *sources]
    else:
        command = [formatter, "--style=file", "-i", *sources]

    return subprocess.call(command)


if __name__ == "__main__":
    sys.exit(main())
