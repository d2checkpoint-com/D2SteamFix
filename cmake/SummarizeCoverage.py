"""Report the union of instrumented production lines without hiding uncovered TUs."""

import argparse
import os
from pathlib import Path
import xml.etree.ElementTree as ET


def line_ranges(lines):
    ranges = []
    for line in sorted(lines):
        if ranges and line == ranges[-1][1] + 1:
            ranges[-1][1] = line
        else:
            ranges.append([line, line])
    return ", ".join(str(start) if start == end else f"{start}-{end}" for start, end in ranges)


def summarize(report, source_root, test_run_failed=False):
    files = {}
    document = ET.parse(report)
    source_roots = [source_root]
    for source in document.findall("./sources/source"):
        root = (source.text or "").replace("\\", "/")
        if len(root) == 2 and root[1] == ":":
            # OpenCppCoverage stores root_name() ("D:") separately from the
            # drive-relative filename. Resolve from D:/, not D:'s current dir.
            root += "/"
        if root:
            source_roots.append(Path(root))
    for item in document.iter("class"):
        source = Path(item.attrib["filename"].replace("\\", "/"))
        candidates = [source] if source.is_absolute() else [root / source for root in source_roots]
        relative = None
        for candidate in candidates:
            try:
                path = candidate.resolve().relative_to(source_root).as_posix()
            except ValueError:
                continue
            if path.startswith(("src/", "include/")):
                relative = path
                break
        if relative is None:
            continue
        lines = files.setdefault(relative, {})
        for line in item.findall("./lines/line"):
            number = int(line.attrib["number"])
            lines[number] = lines.get(number, False) or int(line.attrib["hits"]) > 0

    missing = sorted(
        source.relative_to(source_root).as_posix()
        for source in (source_root / "src").glob("*.cpp")
        if source.relative_to(source_root).as_posix() not in files
    )
    total = sum(len(lines) for lines in files.values())
    covered = sum(sum(lines.values()) for lines in files.values())
    percentage = 100.0 * covered / total if total else 0.0
    complete = not missing and total > 0
    label = "Measured coverage" if complete else "Partial coverage (incomplete denominator)"
    if test_run_failed:
        label = "Coverage from a failed test run; not a passing-suite baseline"
    output = [
        "# Production line coverage",
        "",
        f"{label}: **{percentage:.2f}%** ({covered}/{total} instrumented lines).",
        "",
        "The inventory is built from every production translation unit, including code no test calls. "
        "Only executable lines described by Debug symbols contribute to this measurement; "
        "comments, declarations, and uninstantiated templates do not. This is line coverage, "
        "not branch coverage or validation of live Steam/UAC behavior.",
        "",
        "| File | Covered / executable | Coverage |",
        "| --- | ---: | ---: |",
    ]
    for name, lines in sorted(files.items()):
        hits = sum(lines.values())
        percent = 100.0 * hits / len(lines) if lines else 0.0
        output.append(f"| `{name}` | {hits} / {len(lines)} | {percent:.2f}% |")
    output.extend(["", "## Uncovered executable lines", ""])
    for name, lines in sorted(files.items()):
        uncovered = [number for number, hit in lines.items() if not hit]
        if uncovered:
            output.append(f"- `{name}`: {line_ranges(uncovered)}")
    if missing:
        output.extend(["", "## Missing production translation units", ""])
        output.extend(f"- `{name}`" for name in missing)
        output.extend(["", "The report is incomplete; no coverage threshold is evaluated."])
    return "\n".join(output) + "\n", complete


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--test-run-failed", action="store_true")
    arguments = parser.parse_args()
    summary, complete = summarize(
        arguments.report, arguments.source_root.resolve(), arguments.test_run_failed
    )
    arguments.output.write_text(summary, encoding="utf-8")
    print(summary)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as output:
            output.write(summary)
    return 0 if complete else 1


if __name__ == "__main__":
    raise SystemExit(main())
