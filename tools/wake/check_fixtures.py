#!/usr/bin/env python3
"""Replay existing acoustic PCM fixtures; no TTS generation or model downloading.

Print a JSON acceptance report and return nonzero if any expected result fails.
Known missed positives must stay failed cases, rather than being relabeled negatives.
"""
import argparse
import json
import math
from pathlib import Path
import re
import subprocess
import time


def accept_result(returncode, stdout, expected_hits):
    summaries = re.findall(r"^wake replay frames=(\d+) hits=(\d+) status=(\d+)$", stdout, re.MULTILINE)
    if len(summaries) != 1:
        return {"passed": False, "observed_hits": None, "error": "missing or ambiguous probe summary"}
    frames, hits, status = map(int, summaries[0])
    passed = (frames > 0 and hits == expected_hits and status == returncode and
              returncode == (0 if hits else 2))
    return {"passed": passed, "observed_hits": hits, "frames": frames,
            "probe_status": status}


def fixture_path(value, base):
    if not isinstance(value, str) or not value:
        raise ValueError("fixture paths must be nonempty strings")
    path = Path(value)
    return path.resolve() if path.is_absolute() else (base / path).resolve()


def run_cases(probe, manifest):
    document = json.loads(manifest.read_text(encoding="utf-8"))
    cases = document.get("cases") if isinstance(document, dict) else None
    if not isinstance(cases, list) or not cases:
        raise ValueError("manifest must contain a nonempty cases list")
    prepared = []
    for case in cases:
        if not isinstance(case, dict):
            raise ValueError("each fixture case must be an object")
        expected = case.get("expected_hits")
        if not isinstance(expected, int) or isinstance(expected, bool) or expected < 0:
            raise ValueError("expected_hits must be a nonnegative integer")
        model = fixture_path(case.get("model_dir"), manifest.parent)
        dictionary = fixture_path(case.get("dictionary"), manifest.parent)
        pcm = fixture_path(case.get("pcm"), manifest.parent)
        phrase = case.get("phrase", "你好涂鸦")
        if not isinstance(phrase, str) or not phrase:
            raise ValueError("phrase must be a nonempty UTF-8 string")
        if not model.is_dir() or not dictionary.is_file() or not pcm.is_file():
            raise ValueError(f"model, dictionary, or PCM is missing for {case.get('name', pcm.name)}")
        size = pcm.stat().st_size
        if size == 0 or size % 2:
            raise ValueError(f"PCM must contain nonempty 16-bit samples: {pcm}")
        command = [str(probe), str(model), str(dictionary), phrase, str(pcm)]
        if "threshold" in case:
            threshold = float(case["threshold"])
            if not math.isfinite(threshold) or not 0 < threshold < 1:
                raise ValueError("threshold must be finite and between 0 and 1")
            command.append(str(threshold))
        prepared.append((case, command, size / 32000.0))

    results = []
    for case, command, seconds in prepared:
        start = time.monotonic()
        try:
            completed = subprocess.run(command, capture_output=True, text=True,
                                       timeout=max(15, seconds * 3 + 15))
            result = accept_result(completed.returncode, completed.stdout, case["expected_hits"])
            result.update(returncode=completed.returncode, stdout=completed.stdout[-4000:],
                          stderr=completed.stderr[-4000:])
        except subprocess.TimeoutExpired:
            result = {"passed": False, "observed_hits": None, "error": "probe timeout"}
        result.update(name=case.get("name", Path(command[4]).name),
                      expected_hits=case["expected_hits"], audio_seconds=seconds,
                      elapsed_seconds=time.monotonic() - start)
        results.append(result)
    return {"passed": all(result["passed"] for result in results),
            "cases": results, "total": len(results),
            "passed_cases": sum(result["passed"] for result in results),
            "manifest": str(manifest)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, help="optional NEW JSON report file (never overwritten)")
    args = parser.parse_args()
    try:
        if not args.probe.is_file():
            raise ValueError("wake_probe executable is missing")
        if args.output and args.output.exists():
            raise ValueError("output report already exists")
        result = run_cases(args.probe.resolve(), args.manifest.resolve())
        report = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
        if args.output:
            with args.output.open("x", encoding="utf-8") as file:
                file.write(report)
        print(report, end="")
        return 0 if result["passed"] else 1
    except (OSError, ValueError, TypeError) as error:
        print(json.dumps({"passed": False, "error": str(error)}, ensure_ascii=False))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
