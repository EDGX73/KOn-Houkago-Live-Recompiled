"""Check explicit service labels against a pinned public SDK NID table."""
import json
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
contract = json.loads((root / "analysis/threadman_contracts.json").read_text())
source = (root / "src/boot_probe.cpp").read_text(encoding="utf-8")
labels = re.findall(r'rt\.nids\(\)\.add\("ThreadManForUser",(0x[0-9a-fA-F]+)u,"([^"]+)"\)', source)
checked, absent, errors = [], [], []
for nid, label in labels:
    expected = contract["functions"].get(nid.upper())
    if expected is None:
        absent.append(nid)
    elif label != expected:
        errors.append(f"{nid}: {label}, SDK expects {expected}")
    else:
        checked.append(nid.upper())
if "0X6D212BAC" not in checked or "0X58B1F937" not in checked:
    errors.append("Both WaitSemaCB and PollSema require explicit correct labels")
if errors:
    raise SystemExit("Contract mismatch:\n" + "\n".join(errors))
print(f"PASS: {len(checked)} explicit ThreadMan labels match SDK {contract['revision']}; "
      f"{len(absent)} later-firmware labels are absent from this table and remain outside this check. "
      "This is metadata validation, not execution coverage.")

# Display 5.00 NID documented by SilverSpring/uofw; GetFrameBuf is EEDA2E54 in PSPSDK.
if 'rt.nids().add("sceDisplay",0x77ED8B3Au,"sceDisplayWaitVblankStartMultiCB")' not in source:
    raise SystemExit("Display multi-vblank NID must have its correct explicit wait label")
print("PASS: display multi-vblank NID has the explicit wait label; scheduler behavior tested separately")
