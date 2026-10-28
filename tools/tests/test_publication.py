"""Publication controls: the audit blocks game material, personal data and
secrets, and the public-to-private path mapping is exact. Synthetic data only;
runs on any machine (also a public clone or a CI runner)."""
import importlib.util
import os
from pathlib import Path
import struct
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


audit_public = load("audit_public")
import_public = load("import_public")
# Built at run time so that this file does not contain what the audit blocks.
PROFILE = b"C:" + b"\\" + b"Users" + b"\\"
UNIT = b"recomp_" + b"unit_0012"
LABEL = b"L_" + b"088EAC44"


def findings(data, path="src/sample.cpp", mode="source"):
    audit = audit_public.Audit(mode)
    audit.allowlist = []
    audit.check(path, data)
    return {rule for _, rule, _ in audit.findings}


class AuditTests(unittest.TestCase):
    def test_plain_source_passes(self):
        self.assertEqual(findings(b"int main() { return 0; }\n"), set())

    def test_game_formats_by_magic(self):
        iso = bytearray(0x8010)
        iso[0x8001:0x8006] = b"CD001"
        mips_elf = b"\x7fELF\x01\x01\x01" + bytes(9) + struct.pack("<HH", 2, 8) + bytes(64)
        for data in (bytes(iso), b"CPK " + bytes(64), b"~PSP" + bytes(64), b"\0PBP" + bytes(64),
                     b"\0PSF" + bytes(64), mips_elf):
            self.assertIn("game-magic", findings(data, "docs/notes.txt"))

    def test_game_names_and_folders(self):
        self.assertIn("game-name", findings(b"x", "PSP_GAME/USRDIR/std.cpk"))
        self.assertIn("game-extension", findings(b"x", "a/b.iso"))
        self.assertIn("game-name", findings(b"x", "work/recompiled_corrected/x.cpp"))
        self.assertIn("game-name", findings(b"x", "userdata/ms0/PSP/SAVEDATA/A/B.konpc"))

    def test_translated_game_code(self):
        unit = b"void " + UNIT + b"(Runtime &rt) {}\n" + (LABEL + b":\n") * 10
        self.assertIn("game-code", findings(unit))
        self.assertEqual(findings(b'out << "recomp_unit_" << id;'), set())  # the generator itself

    def test_personal_paths_and_words(self):
        self.assertIn("personal-path", findings(PROFILE + b"somebody\\Documents\\x"))
        self.assertIn("personal-path", findings(b"/c/" + b"Users/somebody/x"))
        self.assertEqual(findings(PROFILE + b"<usuario>\\x and " + PROFILE + b"Public"), set())
        os.environ["KON_AUDIT_EXTRA_WORDS"] = "Zyxwvut"
        try:
            self.assertIn("personal-data", findings(b"made by zyxwvut."))
            self.assertEqual(findings(b"made by zyxwvutsr."), set())  # whole words only
        finally:
            del os.environ["KON_AUDIT_EXTRA_WORDS"]

    def test_secrets_but_not_pem_constants(self):
        self.assertIn("secret", findings(b"token=ghp_" + b"a1" * 20))
        key = b"-----BEGIN RSA PRIVATE KEY-----\nMIIEowIBAAKCAQEAq7BFUpkGp3+XQ3ZYgXRkpxwzVnI2Ff5ZkF\n"
        self.assertIn("secret", findings(key, "notes.txt"))
        constant = b'MZ"-----BEGIN RSA PRIVATE KEY-----"\0'
        self.assertEqual(findings(constant, "pkg/third_party/ffmpeg/bin/avformat-62.dll", "release"), set())

    def test_binaries_in_source_and_release_layout(self):
        self.assertIn("binary-in-source", findings(b"MZ\0\0" + bytes(100), "tools/x.bin"))
        self.assertEqual(findings(b"\x89PNG\r\n\x1a\n\0\0", "assets/branding/icon.png"), set())
        self.assertIn("unexpected-binary", findings(b"MZ" + bytes(10), "pkg/bin/game.exe", "release"))
        game = b"MZ" + bytes(40 << 20)
        self.assertIn("large-binary", findings(game, "pkg/K-On Houkago Live Recompiled.exe", "release"))
        self.assertEqual(findings(b"MZ" + bytes(1000), "pkg/K-On Houkago Live Recompiled.exe", "release"), set())

    def test_allowlist_needs_matching_rule(self):
        audit = audit_public.Audit("source")
        audit.allowlist = [{"path": "tools/patch.py", "rule": "game-code", "reason": "test"}]
        audit.check("tools/patch.py", (LABEL + b":\n") * 10)
        audit.check("tools/patch.py", PROFILE + b"somebody\\x")
        self.assertEqual({r for _, r, _ in audit.findings}, {"personal-path"})
        self.assertEqual(len(audit.allowed), 1)


class ImportMappingTests(unittest.TestCase):
    def test_overlay_files_go_back_to_public_overlay(self):
        patch = ("diff --git a/CLAUDE.md b/CLAUDE.md\n--- a/CLAUDE.md\n+++ b/CLAUDE.md\n@@ -1 +1 @@\n-a\n+b\n"
                 "diff --git a/src/x.cpp b/src/x.cpp\n--- a/src/x.cpp\n+++ b/src/x.cpp\n@@ -1 +1 @@\n-a\n+b\n"
                 "diff --git a/.github/new.yml b/.github/new.yml\nnew file mode 100644\n--- /dev/null\n+++ b/.github/new.yml\n")
        mapped = import_public.map_overlay(patch, {"CLAUDE.md", ".github/new.yml"})
        self.assertIn("diff --git a/public_overlay/CLAUDE.md b/public_overlay/CLAUDE.md\n", mapped)
        self.assertIn("+++ b/public_overlay/CLAUDE.md\n", mapped)
        self.assertIn("diff --git a/src/x.cpp b/src/x.cpp\n", mapped)
        self.assertIn("+++ b/public_overlay/.github/new.yml\n", mapped)
        self.assertIn("--- /dev/null\n", mapped)
        self.assertIn("+b\n", mapped)


if __name__ == "__main__":
    unittest.main()
