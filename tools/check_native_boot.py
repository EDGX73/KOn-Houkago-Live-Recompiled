"""Run real native guest bootstrap, preserving explicit incomplete-port evidence."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import struct
import uuid
import re

ELF_HASH = "eda890135ea4aee1a8d0a82b2f00a7159060c58498a6705df7518b36e46b85b1"
MILESTONES = (
    "module_return_value=0",
    "Running native worker uid=2 name=user_main",
    "HLE: AllocPartitionMemory name=UserSbrk size=20267008 result=0x00001001",
    "HLE: GetBlockHeadAddr=0x08BA8000",
    "HLE: CreateCallback uid=3 name=exit entry=0x0880419C",
    "HLE: RegisterExitCallback uid=3",
    "HLE: SetLanguageMode system_language=0 confirm_button=0",
    "HLE: ChangeCurrentThreadAttr uid=2 clear=0x00000000 set=0x00004000",
    "HLE: RtcGetCurrentTick destination=0x09FEFA30",
    "HLE: CtrlSetSamplingCycle=11111",
    "HLE: GeListFinished id=0 commands=212",
    "HLE: DisplaySetMode width=480 height=272",
    "HLE: DisplaySetFrameBuffer address=0x04044000 stride=512 format=0 sync=1",
    "HLE: GeListFinished id=0 commands=119",
    "HLE: GeFinishCallback id=0",
    "HLE: AudioReserve frames=448 format=0 result=0x00000000",
    "HLE: AudioReserve frames=256 format=0 result=0x00000003",
    "HLE: AtracReinit atrac3=0 atrac3plus=3 native_slots=3 result=0x00000000",
    "HLE: CreateLwMutex uid=34 area=0x08B9F904 name=CRI Wave Mutex result=0x00000000",
    "HLE: AudioOutput channel=1 result=0x00000100 wait_token=0",
    "HLE: AudioOutput channel=1 result=0x00000100 wait_token=1",
    "HLE: DeleteThread uid=33 released_stack=16384",
    "HLE: CancelSema uid=36 requested=-1 restored_count=0 cancelled_waiters=0",
    "HLE: MpegInit native_h264_result=0x00000000",
    "HLE: RtcConvertUtcToLocalTime source=0x090B2560",
    "HLE: RtcSetTick date=",
    "HLE: GeNativeClear vertices=16 total_pixels=130560",
    "HLE: WlanGetSwitchState on",
    "HLE: CtrlReadBufferPositive samples=",
    "HLE: SavedataInitStart mode=0 path=",
    "HLE: SavedataUpdate result=0x00000000 request_result=0x80110307",
    "HLE: SavedataShutdownStart result=0x00000000",
    "HLE: SavedataGetStatus result=0x00000000",

)


def fields(output):
    return dict(line.split("=", 1) for line in output.splitlines()
                if "=" in line and not line.startswith("HLE: "))


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def check(root, executable):
    report = {
        "checked_at": datetime.now(timezone.utc).isoformat(),
        "executable": str(executable),
        "executable_sha256": digest(executable),
        "elf_sha256": digest(root / "analysis/executables/pkon.elf"),
        "playable_port": False,
        "menu_verified": False,
        "song_verified": False,
        "language_menu_verified": False,
        "original_clear_executed": False,
        "original_pcm_output_verified": False,
        "original_controls_read_verified": False,
        "cases": [],
    }
    require(report["elf_sha256"] == ELF_HASH, "Unexpected guest executable")
    outputs = {}
    # Bootstrap absence tests must never inspect or replace a player's saves.
    fixture_storage = root / "work" / ("bootstrap_fixture_" + uuid.uuid4().hex)
    for language, bootstrap in (("en", False), ("ja", True), ("en", True)):
        arguments = [str(executable), "--root", str(root), "--language", language,
                     "--userdata", str(fixture_storage)]
        if bootstrap:
            frame = root / "analysis" / f"native_initial_frame_{language}.bmp"
            arguments.extend(("--bootstrap", "--stop-after-visible-frame", "--capture-frame", str(frame)))
        result = subprocess.run(arguments, cwd=root, capture_output=True,
                                text=True, timeout=30)
        output = result.stdout + result.stderr
        case = f"native_full_{language}_{'bootstrap' if bootstrap else 'no_hle'}"
        (root / "analysis" / f"{case}.log").write_text(output, encoding="utf-8")
        require(result.returncode == 2, f"{case}: unexpected exit {result.returncode}: {output}")
        values = fields(output)
        require("playable_port=false" in output, f"{case}: missing diagnostic label")
        require(values.get("ELF_SHA256") == ELF_HASH, f"{case}: wrong ELF")
        require(values.get("entry") == "0x089001F8", f"{case}: wrong entry")
        require(values.get("imports") == "238", f"{case}: wrong import table")
        locale_dir = "jp" if language == "ja" else "en"
        expected_std = root / "analysis/extracted" / locale_dir / "PSP_GAME/USRDIR/std.cpk"
        expected_sound = root / "analysis/extracted/jp/PSP_GAME/USRDIR/snd.cpk"
        require(Path(values["std"]).resolve() == expected_std.resolve(), f"{case}: wrong locale mount")
        require(Path(values["sound"]).resolve() == expected_sound.resolve(), f"{case}: wrong shared sound")
        stop=values.get("worker_stop",values.get("module_stop", ""))
        if stop == "Diagnostic original visible frame checkpoint":
            require(bootstrap and "HLE: OriginalVisibleFrame nonblack_pixels=" in output, f"{case}: unproven visible checkpoint")
            bitmap = frame.read_bytes()
            require(bitmap[:2] == b"BM" and struct.unpack_from("<ii", bitmap, 18) == (480, -272), f"{case}: invalid native capture")
            offset = struct.unpack_from("<I", bitmap, 10)[0]
            count = sum((pixel[0] & 0xFFFFFF) != 0 for pixel in struct.iter_unpack("<I", bitmap[offset:]))
            require(count >= 32, f"{case}: captured frame is black")
        require((values.get("missing_library") and values.get("missing_nid")) or stop == "Diagnostic original visible frame checkpoint" or
                stop.startswith(("GE primitive requires native renderer", "GE saved context buffer", "GE explicit stack count")),
                f"{case}: unexpected native execution stop: {stop}")
        if bootstrap:
            for milestone in MILESTONES:
                require(milestone in output, f"{case}: missing {milestone}")
            require(values.get("worker_stop"), f"{case}: missing explicit worker stop")
            require(output.count("HLE: PowerRegisterCallback slot=0 uid=8 ") == 1,
                    f"{case}: callback context switch replayed power registration")
            require(output.count("HLE: CallbackExecuted uid=8 ") == 1,
                    f"{case}: initial power callback missing or replayed")
            initialization = output.split("HLE: IoOpen path=", 1)[0]
            power = re.findall(r"HLE: PowerRegisterCallback slot=0 uid=8 host_flags=(0x[0-9A-F]{8})", initialization)
            notified = re.findall(r"HLE: CallbackExecuted uid=8 notifications=1 argument=(0x[0-9A-F]{8}) result=0", initialization)
            require(len(power) == 1 and power == notified, f"{case}: power callback did not receive the registered host state")
            # The real Windows battery percentage can change between JA/EN.
            # Validate delivery above; compare their initialization independent
            # of this one environmental value, retaining all other arguments.
            outputs[language] = []
            for line in initialization.splitlines():
                if not line.startswith("HLE: "):
                    continue
                if line.startswith("HLE: PowerRegisterCallback slot=0 uid=8 "):
                    line = re.sub(r"host_flags=0x[0-9A-F]{8}", "host_flags=<host_power>", line)
                elif line.startswith("HLE: CallbackExecuted uid=8 notifications=1 "):
                    line = re.sub(r"argument=0x[0-9A-F]{8}", "argument=<host_power>", line)
                outputs[language].append(line)
            for archive in ("std.cpk", "snd.cpk"):
                require(f"HLE: IoOpen path=disc0:/PSP_GAME/USRDIR/{archive} flags=0x00004001" in output,
                        f"{case}: original program did not open {archive}")
            require("HLE: IoRead descriptor=3 requested=2048 result=0x00000800" in output,
                    f"{case}: original program did not read localized archive header")
            require("HLE: IoRead descriptor=4 requested=2048 result=0x00000800" in output,
                    f"{case}: original program did not read shared sound header")
        else:
            require(values["missing_library"] == "SysMemUserForUser" and
                    values["missing_nid"] == "0x35669D4C", f"{case}: baseline bypassed missing SDK call")
        report["cases"].append({
            "name": case, "language": language, "bootstrap": bootstrap,
            "exit_code": result.returncode,
            "native_archive_reads": bootstrap,
            "unresolved_library": values.get("missing_library",values.get("stopped_library")),
            "unresolved_nid": values.get("missing_nid",values.get("stopped_nid")),
            "stop": values.get("worker_stop", values.get("module_stop")),
            "log": f"analysis/{case}.log",
        })
    require(outputs["ja"] == outputs["en"], "Language packs diverged before the first localized archive read")
    report["bootstrap_checks_passed"] = True
    report["original_clear_executed"] = True
    report["original_pcm_output_verified"] = True
    report["original_controls_read_verified"] = True
    destination = root / "analysis/native_boot_verification.json"
    destination.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("PASS: native baseline and JA/EN initialization; no menu or playable game yet")
    print(f"Evidence: {destination}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--executable", type=Path)
    args = parser.parse_args()
    task_root = args.root.resolve()
    executable = (args.executable or task_root / "build/native_full/kon_boot_probe.exe").resolve()
    check(task_root, executable)
