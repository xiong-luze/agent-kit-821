#!/usr/bin/env python3
"""Derive compact KWS models from the official Mandarin 5.2 model.

This is a reproducible reduction, not newly trained or board-validated weights.
It removes unused phones/Gaussian rows, optionally retaining exact keyword triphones.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct


SOURCE_URL = ("https://sourceforge.net/projects/cmusphinx/files/Acoustic%20and%20Language%20Models/"
              "Mandarin/cmusphinx-zh-cn-5.2.tar.gz")
SOURCE_SHA256 = {
    "mdef": "1532954d9d723d351f7fd5679fa013a96176c15e2380be37ca28a26c103c07fb",
    "means": "02d2c7114e06edb50923ec0c16286373b384bea75ef8a9966344395bf566b72d",
    "variances": "94ea1a42adbf80441565c8b866a18ce21af58e8d17fbe192d1ccb781f825ad3d",
    "mixture_weights": "5d07f870febec71849385f44d7f197f67b3fca242b61d8f8368e98eb7b687fe8",
}


def phonemes(phrase, lexicon):
    if not phrase or len(phrase.encode("utf-8")) >= 128 or any(c.isspace() for c in phrase):
        raise ValueError("phrase must be a nonempty single UTF-8 word shorter than 128 bytes")
    if any(ord(c) < 32 or ord(c) == 127 for c in phrase):
        raise ValueError("control characters are not allowed in a keyword")
    # Prefer the longest dictionary entries but backtrack if their suffix cannot
    # be segmented. This preserves model-specific pronunciations and sandhi rules.
    best = {len(phrase): []}
    for i in range(len(phrase) - 1, -1, -1):
        for j in range(len(phrase), i, -1):
            if phrase[i:j] in lexicon and j in best:
                best[i] = [lexicon[phrase[i:j]], *best[j]]
                break
    if 0 not in best:
        raise ValueError(f"keyword cannot be pronounced using this model dictionary: {phrase}")
    return " ".join(best[0])


def reduce_gaussian(data, name, ci_states):
    if name not in ("means", "variances", "mixture_weights"):
        raise ValueError("unsupported S3 parameter file")
    if not data.startswith(b"s3\n") or b"endhdr\n" not in data[:4096]:
        raise ValueError("invalid S3 parameter header")
    index = data.index(b"endhdr\n") + len(b"endhdr\n")
    body = data[index:]
    if len(body) < 24:
        raise ValueError("truncated S3 parameter header")
    magic, states, features, densities = struct.unpack_from("<4I", body)
    if magic != 0x11223344 or states != 5576 or features != 1 or densities != 32:
        raise ValueError("expected the little-endian Mandarin 5.2 continuous model")
    state_ids = list(range(ci_states)) if isinstance(ci_states, int) else list(ci_states)
    if (not state_ids or len(set(state_ids)) != len(state_ids) or
            any(not 0 <= value < states for value in state_ids)):
        raise ValueError("invalid CI state count")
    count = len(state_ids)
    if name == "mixture_weights":
        values = struct.unpack_from("<I", body, 16)[0]
        if values != states * features * densities:
            raise ValueError("mixture weight dimensions do not match")
        header = [count, features, densities, count * features * densities]
        offset, width = 20, densities * 4
    else:
        dimensions, values = struct.unpack_from("<2I", body, 16)
        if dimensions != 36 or values != states * densities * dimensions:
            raise ValueError("Gaussian dimensions do not match")
        header = [count, features, densities, dimensions, count * densities * dimensions]
        offset, width = 24, densities * dimensions * 4
    checksum_bytes = 4 if b"chksum0" in data[:index] else 0
    if len(body) != offset + states * width + checksum_bytes:
        raise ValueError("truncated or unexpected S3 parameter payload")
    # The derived file has a new dimension header. Omit the optional old checksum
    # rather than carrying a checksum that covered the original payload.
    payload = b"".join(body[offset + state * width:offset + (state + 1) * width]
                       for state in state_ids)
    return (b"s3\nversion 1.0\nendhdr\n" + struct.pack("<I", magic) +
            struct.pack("<" + "I" * len(header), *header) + payload)


def select_inventory(rows, required, compact):
    if not compact:
        return rows
    groups = {}
    for row in rows:
        groups.setdefault(re.sub(r"[1-5]$", "", row[0]), []).append(row)
    names = set()
    for group in groups.values():
        # Retain every exact keyword tone; use one exemplar for other tone
        # variants so background speech still has every Mandarin base phone.
        selected = [row for row in group if row[0] in required] or group[:1]
        names.update(row[0] for row in selected)
    return [row for row in rows if row[0] in names]


def select_keyword_triphones(rows, phones):
    contexts = {(center, left, right)
                for left, center, right in zip(["SIL"] + phones, phones, phones[1:] + ["SIL"])}
    return [row for row in rows if tuple(row[:3]) in contexts]


def prepare(source, output, phrase, compact=True, keyword_triphones=False):
    acoustic = source / "zh_cn.cd_cont_5000"
    actual_sha256 = {name: hashlib.sha256((acoustic / name).read_bytes()).hexdigest()
                     for name in SOURCE_SHA256}
    if actual_sha256 != SOURCE_SHA256:
        raise ValueError("source model differs from the pinned Mandarin 5.2 parameters")
    lexicon = {}
    for line in (source / "zh_cn.dic").read_text(encoding="utf-8").splitlines():
        parts = line.split(maxsplit=1)
        if len(parts) == 2 and not parts[0].endswith(")"):
            lexicon[parts[0]] = parts[1]
    phones = phonemes(phrase, lexicon)
    lines = (acoustic / "mdef").read_text(encoding="utf-8").splitlines()
    rows = [line.split() for line in lines[7:] if line.strip() and not line.startswith("#")]
    ci = [row for row in rows if row[1:4] == ["-", "-", "-"]]
    ci_states = [int(value) for row in ci for value in row[6:-1]]
    if len(ci) != 192 or sorted(set(ci_states)) != list(range(576)):
        raise ValueError("expected Mandarin 5.2 CI phones with contiguous 0..575 senones")
    phone_names = {row[0] for row in ci}
    if not set(phones.split()) <= phone_names:
        raise ValueError("keyword dictionary contains a phone absent from the acoustic model")

    selected = select_inventory(ci, set(phones.split()) | {"SIL"}, compact)
    triphones = select_keyword_triphones(rows, phones.split()) if keyword_triphones else []
    state_ids = []
    remap = {}
    out_rows = []
    for row in selected + triphones:
        original = [int(value) for value in row[6:-1]]
        for value in original:
            if value not in remap:
                remap[value] = len(state_ids)
                state_ids.append(value)
        out_rows.append(" ".join(row[:6] + [str(remap[value]) for value in original] + ["N"]))
    # Validate source payloads and keyword before creating any output.
    reduced = {name: reduce_gaussian((acoustic / name).read_bytes(), name, state_ids)
               for name in ("means", "variances", "mixture_weights")}
    output.mkdir(parents=True, exist_ok=False)
    header = ["0.3", f"{len(selected)} n_base", f"{len(triphones)} n_tri",
              f"{(len(selected) + len(triphones)) * 4} n_state_map",
              f"{len(state_ids)} n_tied_state", f"{len(selected) * 3} n_tied_ci_state", "192 n_tied_tmat"]
    (output / "mdef").write_text("\n".join(header + out_rows) + "\n", encoding="utf-8")
    for name, payload in reduced.items():
        (output / name).write_bytes(payload)
    for name in ("transition_matrices", "feature_transform", "noisedict"):
        shutil.copy2(acoustic / name, output / name)
    features = (acoustic / "feat.params").read_text().replace("-cmn batch", "-cmn live")
    (output / "feat.params").write_text(features)
    (output / "wake.dict").write_text(f"{phrase} {phones}\n", encoding="utf-8")
    shutil.copy2(source / "README", output / "LICENSE.model.txt")
    kind = "compact" if compact else "full"
    kind += " CI+keyword-triphone" if keyword_triphones else " CI-only"
    manifest = {"source_url": SOURCE_URL,
                "derived_kind": kind + " KWS experiment, not retrained",
                "phrase": phrase, "phones": phones,
                "base_phones": len(selected), "tied_states": len(state_ids),
                "compact_tone_inventory": compact,
                "keyword_triphones": len(triphones),
                "source_sha256": actual_sha256,
                "dictionary_source": "Mandarin 5.2 lexicon derived from CC-CEDICT; see retained model license",
                "dictionary_attribution": "https://www.mdbg.net/chinese/dictionary?page=cc-cedict"}
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    manifest["output_bytes"] = sum(path.stat().st_size for path in output.iterdir() if path.is_file())
    print(json.dumps(manifest, ensure_ascii=False, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True,
                        help="unpacked official cmusphinx-zh-cn-5.2 directory")
    parser.add_argument("--output", type=Path, required=True, help="new output directory (never overwritten)")
    parser.add_argument("--phrase", default="你好涂鸦")
    parser.add_argument("--full-ci", action="store_true", help="retain all 192 tone phones for a high-memory host baseline")
    parser.add_argument("--keyword-triphones", action="store_true",
                        help="candidate: retain original triphones matching the exact keyword chain with SIL boundaries")
    args = parser.parse_args()
    try:
        prepare(args.source, args.output, args.phrase, compact=not args.full_ci,
                keyword_triphones=args.keyword_triphones)
    except (OSError, ValueError) as error:
        parser.exit(1, f"model preparation failed: {error}\n")


if __name__ == "__main__":
    main()
