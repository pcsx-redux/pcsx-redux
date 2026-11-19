#!/usr/bin/env python3
# Renders every JUnit XML file under a directory as a markdown table, for
# $GITHUB_STEP_SUMMARY. Usage: test-summary.py <results-dir> [title]
# A file that does not parse (a run that crashed mid-write) is reported as a
# failure, never skipped.

import html
import os
import sys
import xml.etree.ElementTree as ET


def cell(text):
    text = " ".join((text or "").split())
    if len(text) > 300:
        text = text[:297] + "..."
    return html.escape(text).replace("|", "\\|")


def outcome(case):
    for tag, label in (("failure", "FAIL"), ("error", "ERROR")):
        node = case.find(tag)
        if node is not None:
            return label, node.get("message") or node.text or ""
    skipped = case.find("skipped")
    if skipped is not None:
        return "SKIP", skipped.get("message") or skipped.text or ""
    if case.get("result") == "skipped" or case.get("status") == "notrun":
        return "SKIP", ""
    return "PASS", ""


def collect(directory):
    rows = []
    for root, _, files in os.walk(directory):
        for name in sorted(files):
            if not name.endswith(".xml"):
                continue
            path = os.path.join(root, name)
            source = os.path.relpath(path, directory)
            try:
                tree = ET.parse(path)
            except ET.ParseError as e:
                rows.append((source, source, "ERROR", "", f"unreadable result file: {e}"))
                continue
            for suite in tree.getroot().iter("testsuite"):
                for case in suite.findall("testcase"):
                    status, message = outcome(case)
                    group = case.get("classname") or suite.get("name") or ""
                    rows.append((source, group, status, case.get("name") or "", message))
    return rows


def main():
    directory = sys.argv[1]
    title = sys.argv[2] if len(sys.argv) > 2 else "Test results"
    rows = collect(directory) if os.path.isdir(directory) else []
    out = [f"## {title}", ""]
    if not rows:
        out.append(f"No test results found in `{directory}`.")
        print("\n".join(out))
        return
    counts = {}
    for row in rows:
        counts[row[2]] = counts.get(row[2], 0) + 1
    out.append(", ".join(f"{counts.get(k, 0)} {k.lower()}" for k in ("PASS", "FAIL", "ERROR", "SKIP")))
    out.append("")
    header = ["| Result | Suite | Test | Message |", "|---|---|---|---|"]
    bad = [r for r in rows if r[2] in ("FAIL", "ERROR")]
    if bad:
        out.append("### Failures")
        out.append("")
        out += header
        out += [f"| {r[2]} | {cell(r[1])} | {cell(r[3])} | {cell(r[4])} |" for r in bad]
        out.append("")
    out.append(f"<details><summary>All {len(rows)} tests</summary>")
    out.append("")
    out += header
    out += [f"| {r[2]} | {cell(r[1])} | {cell(r[3])} | {cell(r[4])} |" for r in rows]
    out.append("")
    out.append("</details>")
    print("\n".join(out))


if __name__ == "__main__":
    main()
