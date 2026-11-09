#!/usr/bin/env python3
"""T1, step 2: compile every pass written by fxdump with DXC to SPIR-V.

Usage:
    run_t1.py --dxc <dxc dir> --src <fxdump output> --effects <src/Effects> --out <dir>

Writes <out>/results.tsv (one row per pass and configuration), <out>/errors.txt
(full compiler output of every failure) and prints a summary.
"""

import argparse
import collections
import concurrent.futures
import csv
import hashlib
import os
import pathlib
import re
import subprocess
import time

# Resource registers start at 0 for every class in HLSL (t0, s0, u0, b0). In
# SPIR-V they share one binding namespace per set, so each class gets a range.
COMMON_ARGS = [
    "-E", "__M",
    "-spirv",
    "-fspv-target-env=vulkan1.3",
    "-O3",
    "-fvk-t-shift", "0", "0",
    "-fvk-s-shift", "32", "0",
    "-fvk-u-shift", "48", "0",
    "-fvk-b-shift", "64", "0",
]

def fp16_rows(row):
    return row["variant"] == "default" and row["fp16"] == "1"


def float32_textures(source):
    """Declare textures with 32-bit texel types; shader code keeps using MF."""
    return re.sub(
        r"^(RW)?Texture2D<((?:unorm |snorm )?)MF(\d?)>",
        r"\1Texture2D<\2float\3>",
        source, flags=re.MULTILINE,
    )


# name -> (extra arguments, which manifest rows it applies to, source transform)
CONFIGS = {
    # Main question: FXC-like language rules, min16float as relaxed precision.
    "hv2018": (["-T", "cs_6_0", "-HV", "2018"], lambda row: True, None),
    # min16float as real 16-bit floats, only where the effect uses FP16.
    "hv2018-16bit": (
        ["-T", "cs_6_2", "-HV", "2018", "-enable-16bit-types"], fp16_rows, None,
    ),
    # The same, with texture declarations changed to 32-bit texel types.
    "hv2018-16bit-f32tex": (
        ["-T", "cs_6_2", "-HV", "2018", "-enable-16bit-types"], fp16_rows, float32_textures,
    ),
    # DXC's default language version, for information.
    "hv2021": (["-T", "cs_6_0", "-HV", "2021"], lambda row: row["variant"] == "default", None),
}


def read_defs(path):
    defs = []
    for line in path.read_text().splitlines():
        if line:
            name, _, value = line.partition("=")
            defs.append(f"-D{name}={value}" if value else f"-D{name}")
    return defs


def compile_one(job):
    dxc, lib_dir, args, out_file = job
    out_file.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, LD_LIBRARY_PATH=str(lib_dir))
    start = time.monotonic()
    try:
        proc = subprocess.run(
            [str(dxc), *args, "-Fo", str(out_file)],
            capture_output=True, text=True, env=env, timeout=600,
        )
        code, output = proc.returncode, proc.stdout + proc.stderr
    except subprocess.TimeoutExpired:
        code, output = -1, "error: timed out after 600 s"
    return code, output, time.monotonic() - start


def error_key(output):
    """First error line with locations and identifiers removed, for grouping."""
    for line in output.splitlines():
        if "error" in line:
            line = re.sub(r"^.*?:\d+:\d+: ", "", line)
            line = re.sub(r"'[^']*'", "'…'", line)
            return line.strip()
    return output.strip().splitlines()[0] if output.strip() else "(no output)"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True, type=pathlib.Path)
    parser.add_argument("--src", required=True, type=pathlib.Path)
    parser.add_argument("--effects", required=True, type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--jobs", type=int, default=os.cpu_count())
    opts = parser.parse_args()

    dxc = opts.dxc / "bin" / "dxc"
    lib_dir = opts.dxc / "lib"

    with open(opts.src / "manifest.tsv", newline="") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))

    # Identical source + arguments are compiled once and shared.
    unique = {}
    tasks = []
    for config, (config_args, applies, transform) in CONFIGS.items():
        for row in rows:
            if not applies(row):
                continue
            source = opts.src / row["file"]
            defs = read_defs(source.with_suffix(".defs"))
            if transform:
                transformed = opts.out / "transformed" / config / row["file"]
                transformed.parent.mkdir(parents=True, exist_ok=True)
                transformed.write_text(transform(source.read_text()))
                source = transformed
            subdir = pathlib.PurePosixPath(row["effect"].replace("\\", "/")).parent
            args = [
                *COMMON_ARGS, *config_args,
                "-I", str(opts.effects / subdir),
                *defs,
            ]
            digest = hashlib.sha256(
                source.read_bytes() + "\0".join(args).encode()
            ).hexdigest()
            if digest not in unique:
                out_file = opts.out / "spv" / config / pathlib.Path(row["file"]).with_suffix(".spv")
                unique[digest] = (dxc, lib_dir, [*args, str(source)], out_file)
            tasks.append((config, row, digest))

    print(f"{len(tasks)} pass compilations, {len(unique)} unique, {opts.jobs} jobs")

    start = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(opts.jobs) as pool:
        digests = list(unique)
        outcomes = dict(zip(digests, pool.map(compile_one, (unique[d] for d in digests))))
    print(f"compiled in {time.monotonic() - start:.0f} s\n")

    opts.out.mkdir(parents=True, exist_ok=True)
    stats = collections.defaultdict(lambda: {"ok": 0, "fail": 0, "warn": 0, "effects": {}})
    groups = collections.defaultdict(lambda: collections.defaultdict(list))

    with open(opts.out / "results.tsv", "w", newline="") as results, \
            open(opts.out / "errors.txt", "w") as errors:
        writer = csv.writer(results, delimiter="\t")
        writer.writerow(["config", "variant", "effect", "pass", "ok", "warnings", "seconds", "first_error"])

        for config, row, digest in tasks:
            code, output, seconds = outcomes[digest]
            ok = code == 0
            warnings = output.count("warning:")
            key = (config, row["variant"])
            stat = stats[key]
            stat["ok" if ok else "fail"] += 1
            stat["warn"] += warnings
            stat["effects"][row["effect"]] = stat["effects"].get(row["effect"], True) and ok

            first_error = ""
            if not ok:
                first_error = error_key(output)
                groups[key][first_error].append(f'{row["effect"]} pass {row["pass"]}')
                errors.write(f'===== [{config}/{row["variant"]}] {row["effect"]} pass {row["pass"]}\n{output}\n')

            writer.writerow([config, row["variant"], row["effect"], row["pass"],
                             int(ok), warnings, f"{seconds:.2f}", first_error])

    for (config, variant), stat in stats.items():
        effects_ok = sum(stat["effects"].values())
        print(f'[{config} / {variant}] passes: {stat["ok"]} ok, {stat["fail"]} failed; '
              f'effects: {effects_ok} of {len(stat["effects"])} compile completely; '
              f'{stat["warn"]} warnings')
        for message, where in sorted(groups[(config, variant)].items(), key=lambda kv: -len(kv[1])):
            print(f"    {len(where):4d} x {message}")
            print(f"           e.g. {where[0]}")
        print()

    slowest = sorted(outcomes.values(), key=lambda o: -o[2])[:3]
    print("slowest compilations (s):", ", ".join(f"{o[2]:.1f}" for o in slowest))


if __name__ == "__main__":
    main()
