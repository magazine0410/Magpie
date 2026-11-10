#!/usr/bin/env python3
"""Converts the .resw files of the Windows user interface to a Qt .ts file.

The names of the texts are kept as message ids, so the Qt application asks
for a text with the name it has on Windows (qsTrId("Home_PageFrame.Title")),
and a translation contributed for the Windows version is used here without
further work.

Several .resw files of one language may be given; a later file overrides an
earlier one. The texts that only the Linux application has are in files of
their own next to this script.

Usage: resw2ts.py --language de --source EN.resw [--source EN2.resw]
                  --output magpie_de.ts FILE.resw [FILE.resw ...]

The --source files give the English text of every message, which Qt's tools
show to translators. Only messages that the sources have are written.
"""
import argparse
import sys
import xml.etree.ElementTree as ElementTree
from xml.sax.saxutils import escape, quoteattr


def read_resw(path):
    texts = {}
    for data in ElementTree.parse(path).getroot().iter("data"):
        name = data.get("name")
        value = data.find("value")
        if name and value is not None and value.text:
            texts[name] = value.text
    return texts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--language", required=True)
    parser.add_argument("--source", action="append", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("files", nargs="*")
    arguments = parser.parse_args()

    sources = {}
    for path in arguments.source:
        sources.update(read_resw(path))
    translations = {}
    for path in arguments.files:
        translations.update(read_resw(path))

    lines = [
        '<?xml version="1.0" encoding="utf-8"?>',
        "<!DOCTYPE TS>",
        f"<TS version=\"2.1\" language={quoteattr(arguments.language)}>",
        # Messages that are found by their id have a context without a name
        "<context>",
        "    <name></name>",
    ]
    for name in sorted(sources):
        translation = translations.get(name)
        lines.append(f"    <message id={quoteattr(name)}>")
        lines.append(f"        <source>{escape(sources[name])}</source>")
        if translation:
            lines.append(f"        <translation>{escape(translation)}</translation>")
        else:
            lines.append('        <translation type="unfinished"></translation>')
        lines.append("    </message>")
    lines += ["</context>", "</TS>", ""]

    with open(arguments.output, "w", encoding="utf-8") as file:
        file.write("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
