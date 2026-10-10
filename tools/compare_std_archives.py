"""Compare decoded std resources and list nested changed FMDX members."""
from __future__ import annotations
import argparse
import csv
import json
from pathlib import Path
from cpk_archive import FormatError, safe_destination, write_json
from fmdx_archive import FmdxArchive


def compare_maps(jp, en):
    result = {"identical": [], "changed": [], "only_jp": [], "only_en": []}
    for path in sorted(set(jp) | set(en)):
        left, right = jp.get(path), en.get(path)
        if left is None:
            result["only_en"].append(right)
        elif right is None:
            result["only_jp"].append(left)
        elif left["sha256"] == right["sha256"]:
            result["identical"].append({"path": path, "sha256": left["sha256"], "size": left["extracted_size"]})
        else:
            result["changed"].append({"path": path, "jp_sha256": left["sha256"], "en_sha256": right["sha256"], "jp_size": left["extracted_size"], "en_size": right["extracted_size"]})
    result["summary"] = {key: len(value) for key, value in result.items()}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    manifests = {lang: json.loads((root / f"analysis/std_{lang}_manifest.json").read_text(encoding="utf-8")) for lang in ("jp", "en")}
    entries = {lang: {e["path"]: e for e in m["entries"]} for lang, m in manifests.items()}
    result = compare_maps(entries["jp"], entries["en"])
    result["source_manifests"] = {lang: f"analysis/std_{lang}_manifest.json" for lang in manifests}
    result["excluded"] = [{"path": "PSP_GAME/padding.bin", "reason": "Disc fill; extraction selected PSP_GAME/USRDIR/* only"}]
    nested = []
    candidates = sorted({e["path"] for group in ("changed", "only_jp", "only_en") for e in result[group] if e["path"].lower().endswith(".bin")})
    for path in candidates:
        indices = {}
        references = {}
        for lang in ("jp", "en"):
            if path not in entries[lang]:
                continue
            file = safe_destination(root / f"assets/source/{lang}/std", path)
            with file.open("rb") as source:
                magic = source.read(4)
            if magic != b"FMDX":
                continue
            archive = FmdxArchive(file)
            listing = archive.metadata(hashes=True)
            out = safe_destination(root / f"analysis/fmdx/{lang}", path + ".json")
            write_json(out, listing)
            references[lang] = out.relative_to(root).as_posix()
            indices[lang] = {e["path"]: e for e in listing["entries"]}
        if not indices:
            continue
        comparison = compare_maps(indices.get("jp", {}), indices.get("en", {}))
        comparison.update({"archive_path": path, "listings": references})
        nested.append(comparison)
    result["nested_fmdx"] = nested
    result["nested_summary"] = {"archives": len(nested), **{k: sum(n["summary"][k] for n in nested) for k in ("identical", "changed", "only_jp", "only_en")}}
    out = root / "analysis/std_comparison.json"
    write_json(out, result)
    csvpath = root / "analysis/std_comparison.csv"
    with csvpath.open("w", newline="", encoding="utf-8-sig") as sink:
        writer = csv.DictWriter(sink, fieldnames=("path", "status", "jp_sha256", "en_sha256", "jp_size", "en_size"))
        writer.writeheader()
        for group in ("identical", "changed", "only_jp", "only_en"):
            for entry in result[group]:
                path = entry["path"]
                left, right = entries["jp"].get(path, {}), entries["en"].get(path, {})
                writer.writerow({"path": path, "status": group, "jp_sha256": left.get("sha256", ""), "en_sha256": right.get("sha256", ""), "jp_size": left.get("extracted_size", ""), "en_size": right.get("extracted_size", "")})
    print(json.dumps({"summary": result["summary"], "nested_summary": result["nested_summary"], "output": str(out)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
