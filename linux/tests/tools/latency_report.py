#!/usr/bin/env python3
"""Reads the logs of a latency measurement and prints how long frames took.

The source is the program magpie_latency_source, which numbers its frames and
notes when it drew each one and when the compositor showed it. The scaler
(magpie-scale or magpie-engine with the environment variable
MAGPIE_LATENCY_LOG) notes for every frame it scaled the number it read from
the picture and the times of the frame's way through it, up to the moment the
compositor showed the scaled picture.

Usage:
  latency_report.py SOURCE_LOG                the source alone, as it is shown
                                              on the output its window is on
  latency_report.py SOURCE_LOG MAGPIE_LOG     the scaled picture
  latency_report.py --baseline DIRECT_SOURCE_LOG SOURCE_LOG MAGPIE_LOG
                                              also the difference to the
                                              source shown without scaling

All durations are printed in milliseconds as minimum / median / 95th
percentile / maximum.
"""
import sys


def parse(path):
    """The lines of a log as dictionaries; clicks have the key "click"."""
    records = []
    with open(path) as file:
        for line in file:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            record = {}
            for part in line.split():
                if "=" in part:
                    key, value = part.split("=", 1)
                    record[key] = value
                else:
                    record[part] = True
            records.append(record)
    return records


def stats(values):
    if not values:
        return "no values"
    values = sorted(values)
    count = len(values)
    return "%7.2f / %7.2f / %7.2f / %7.2f" % (
        values[0] / 1e6, values[count // 2] / 1e6, values[min(count - 1, count * 95 // 100)] / 1e6, values[-1] / 1e6)


def median(values):
    values = sorted(values)
    return values[len(values) // 2] if values else None


def flags_text(records):
    """How many of the shown frames had which flags of presentation-time."""
    shown = [int(r["flags"]) for r in records if r.get("presented", "discarded") != "discarded" and "flags" in r]
    if not shown:
        return "none shown"
    names = ((1, "synchronised with the refresh"), (8, "without a copy (direct scan-out)"))
    return ", ".join("%s: %d of %d" % (name, sum(1 for f in shown if f & bit), len(shown)) for bit, name in names)


def source_report(records, label):
    frames = [r for r in records if "frame" in r and "drawn" in r]
    shown = [r for r in frames if r.get("presented", "discarded") != "discarded"]
    print("%s: %d frames drawn, %d shown, %d replaced before they were shown" % (
        label, len(frames), len(shown), sum(1 for r in frames if r.get("presented") == "discarded")))
    if len(frames) > 1:
        seconds = (int(frames[-1]["drawn"]) - int(frames[0]["drawn"])) / 1e9
        if seconds > 0:
            print("  drawn per second:       %.1f" % ((len(frames) - 1) / seconds))
    delays = [int(r["presented"]) - int(r["drawn"]) for r in shown]
    print("  drawn to shown:         %s" % stats(delays))
    print("  %s" % flags_text(shown))
    return median(delays)


def main():
    arguments = sys.argv[1:]
    baseline = None
    if arguments[:1] == ["--baseline"] and len(arguments) >= 2:
        baseline = parse(arguments[1])
        arguments = arguments[2:]
    if len(arguments) not in (1, 2):
        print(__doc__)
        return 2

    source = parse(arguments[0])
    baseline_delay = source_report(baseline, "source without scaling") if baseline else None
    if len(arguments) == 1:
        source_report(source, "source")
        return 0

    drawn = {int(r["frame"], 16): int(r["drawn"]) for r in source if "frame" in r and "drawn" in r}
    magpie = [r for r in parse(arguments[1]) if "frame" in r]
    known = [r for r in magpie if int(r["frame"], 16) in drawn]
    shown = [r for r in known if r["presented"] != "discarded"]
    print("scaled picture: %d frames scaled, %d of them with a number of the source, %d shown, %d replaced" % (
        len(magpie), len(known), len(shown), len(known) - len(shown)))
    if not shown:
        return 1

    first, last = int(shown[0]["frame"], 16), int(shown[-1]["frame"], 16)
    drawn_meanwhile = sum(1 for number in drawn if first <= number <= last)
    distinct = len({int(r["frame"], 16) for r in shown})
    print("  frames of the source that were shown: %d of %d (%.1f %%)" % (
        distinct, drawn_meanwhile, 100.0 * distinct / max(drawn_meanwhile, 1)))
    seconds = (int(shown[-1]["presented"]) - int(shown[0]["presented"])) / 1e9
    if seconds > 0:
        print("  shown per second:       %.1f" % ((len(shown) - 1) / seconds))

    def spans(start, end):
        result = []
        for r in shown:
            begin = drawn[int(r["frame"], 16)] if start == "drawn" else int(r[start])
            result.append(int(r[end]) - begin)
        return result

    print("  drawn to captured:      %s" % stats(spans("drawn", "arrival")))
    print("  captured to rendering:  %s" % stats(spans("arrival", "render")))
    print("  rendering:              %s" % stats(spans("render", "queued")))
    print("  handed over to shown:   %s" % stats(spans("queued", "presented")))
    total = spans("drawn", "presented")
    print("  drawn to shown:         %s" % stats(total))
    if baseline_delay is not None:
        print("  added by scaling:       %7.2f (difference of the medians)" % ((median(total) - baseline_delay) / 1e6))
    print("  %s" % flags_text(shown))

    # A click is answered by the first frame with at least the number noted
    clicks = []
    for click in (r for r in source if "click" in r):
        answer = int(click["frame"], 16)
        times = [int(r["presented"]) for r in shown if int(r["frame"], 16) >= answer]
        if times:
            clicks.append(min(times) - int(click["seen"]))
    if clicks:
        print("  click seen to answer shown (%d clicks): %s" % (len(clicks), stats(clicks)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
