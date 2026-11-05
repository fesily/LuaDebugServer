#!/usr/bin/env python3
"""Install the published extension into a VS Code extension directory.

Replaces the old `compile/copy.lua` (which needed bee's Lua bindings). It copies
`publish/` into the target directory, skipping what must never be shipped or
overwritten:

  * `*.log` files and the `tmp/` directory (adapter runtime droppings),
  * the MSVC/Windows runtime DLLs (the distribution is built with the static CRT
    and must not shadow the ones VS Code ships),
  * macOS code signatures: files are removed before being written, because a
    copy over a signed file invalidates the signature of the whole bundle.

Usage:
  tools/install_extension.py <publish-dir> <extension-install-dir>
"""

import argparse
import os
import shutil
import sys

# Names that belong to the runtime, not to the extension: copying any of them
# would shadow the ones the editor already ships.
CRT_PREFIXES = ("api-ms-win-", "msvcp", "vcruntime")
CRT_NAMES = ("concrt140.dll", "ucrtbase.dll")


def is_runtime_dll(filename):
    lowered = filename.lower()
    return lowered.startswith(CRT_PREFIXES) or lowered in CRT_NAMES


def copy_tree(source, destination, platform):
    os.makedirs(destination, exist_ok=True)
    for entry in sorted(os.listdir(source)):
        source_path = os.path.join(source, entry)
        destination_path = os.path.join(destination, entry)
        if entry == "tmp" or entry.endswith(".log"):
            continue
        if os.path.isdir(source_path):
            copy_tree(source_path, destination_path, platform)
            continue
        if is_runtime_dll(entry):
            print("skip runtime dll %s" % entry)
            continue
        print("copy %s -> %s" % (source_path, destination_path))
        if platform == "macos" and os.path.exists(destination_path):
            # A copy over a signed file breaks the bundle signature.
            os.remove(destination_path)
        shutil.copyfile(source_path, destination_path)
        shutil.copymode(source_path, destination_path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("publish_dir")
    parser.add_argument("install_dir")
    parser.add_argument(
        "--platform",
        default="darwin" if sys.platform == "darwin" else "linux" if sys.platform.startswith("linux") else "windows",
        choices=("darwin", "linux", "windows"),
        help="host family, only used for the macOS code-signature rule")
    args = parser.parse_args()

    publish_dir = os.path.abspath(args.publish_dir)
    install_dir = os.path.abspath(args.install_dir)
    if not os.path.isdir(publish_dir):
        print("publish directory not found: %s" % publish_dir, file=sys.stderr)
        return 1
    copy_tree(publish_dir, install_dir, args.platform)
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
