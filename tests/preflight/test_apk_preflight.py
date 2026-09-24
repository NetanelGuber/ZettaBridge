"""Synthetic, unsigned structural fixtures; signer identity is mocked here.

Real apksigner and binary AXML are exercised separately in the Step 02 evidence run.
"""

from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import apk_preflight as p


def manifest(package="test.fixture", version="1", split="", extra=""):
    return (f'<manifest xmlns:android="http://schemas.android.com/apk/res/android" '
            f'package="{package}" android:versionCode="{version}"'
            f'{f" split={split!r}" if split else ""}>'
            f'<uses-sdk android:minSdkVersion="23" android:targetSdkVersion="35"/>'
            f'{extra}<application><activity android:name=".Main"/></application></manifest>').encode()


def make_zip(path, entries, compression=zipfile.ZIP_DEFLATED):
    with zipfile.ZipFile(path, "w", compression=compression) as z:
        for name, data in entries:
            z.writestr(name, data)


class PreflightTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.sign = patch.object(p, "signer", side_effect=lambda path, _: {
            "cert_sha256": ["aa" if "wrong" not in path.name else "bb"],
            "verified_schemes": ["v2 scheme"]})
        self.sign.start()
        self.addCleanup(self.sign.stop)

    def apk(self, name, xml=None, more=()):
        path = self.root / name
        make_zip(path, [("AndroidManifest.xml", xml or manifest()), *more])
        return path

    def test_java_only_and_source_unchanged(self):
        source = self.apk("base.apk")
        before = source.read_bytes()
        result = p.analyze([source], "unused", "unused")
        self.assertEqual(result["status"], "analyzed")
        self.assertEqual(result["package"], "test.fixture")
        self.assertEqual(result["files"][0]["manifest"]["min_sdk"], "23")
        self.assertEqual(source.read_bytes(), before)

    def test_splits_and_container(self):
        base = self.apk("base.apk", manifest(extra='<uses-split android:name="feature"/>'))
        feature = self.apk("feature.apk", manifest(split="feature", extra='<uses-split android:name="base"/>'))
        self.assertEqual(p.analyze([base, feature], "unused", "unused")["split_names"], ["feature"])
        with self.assertRaisesRegex(p.Invalid, "missing required split"):
            p.analyze([base], "unused", "unused")
        container = self.root / "fixture.apks"
        make_zip(container, [("splits/base.apk", base.read_bytes()), ("splits/feature.apk", feature.read_bytes())])
        result = p.analyze([container], "unused", "unused")
        self.assertEqual(result["split_names"], ["feature"])
        self.assertIn("fixture.apks!splits/base.apk", result["files"][0]["file"])

    def test_mixed_identity_and_ambiguous_set(self):
        base = self.apk("base.apk")
        version = self.apk("version.apk", manifest(version="2", split="feature"))
        with self.assertRaisesRegex(p.Invalid, "mixed package, version or signer"):
            p.analyze([base, version], "unused", "unused")
        wrong = self.apk("wrong.apk", manifest(split="feature"))
        with self.assertRaisesRegex(p.Invalid, "mixed package, version or signer"):
            p.analyze([base, wrong], "unused", "unused")
        second = self.apk("second.apk")
        with self.assertRaisesRegex(p.Invalid, "exactly one base"):
            p.analyze([base, second], "unused", "unused")

    def test_bad_zip_paths_duplicates_symlink_bomb_and_manifest(self):
        for name in ("../escape", "C:/escape", "a//b"):
            source = self.apk("bad.apk", more=[(name, b"x")])
            with self.assertRaises(p.Invalid, msg=name):
                p.analyze([source], "unused", "unused")
        with self.assertRaises(p.Invalid):
            p.check_name("a\\b")
        source = self.root / "duplicate.apk"
        with zipfile.ZipFile(source, "w") as z:
            z.writestr("AndroidManifest.xml", manifest())
            z.writestr("AndroidManifest.xml", manifest())
        with self.assertRaisesRegex(p.Invalid, "duplicate"):
            p.analyze([source], "unused", "unused")
        source = self.root / "symlink.apk"
        with zipfile.ZipFile(source, "w") as z:
            z.writestr("AndroidManifest.xml", manifest())
            info = zipfile.ZipInfo("link")
            info.create_system = 3
            info.external_attr = 0o120777 << 16
            z.writestr(info, b"target")
        with self.assertRaisesRegex(p.Invalid, "symlink"):
            p.analyze([source], "unused", "unused")
        source = self.apk("bomb.apk", more=[("assets/huge", b"x" * (8 * 1024 * 1024))])
        with self.assertRaisesRegex(p.Invalid, "extreme compression"):
            p.analyze([source], "unused", "unused")
        source = self.apk("badmanifest.apk", b"<manifest")
        with self.assertRaisesRegex(p.Invalid, "malformed manifest"):
            p.analyze([source], "unused", "unused")

    def test_classification_and_bad_elf(self):
        fake_arm = b"\x7fELF" + b"\x01\x01" + b"\x00" * 12 + b"\x28\x00" + b"\x00" * 40
        fake_x86 = b"\x7fELF" + b"\x01\x01" + b"\x00" * 12 + b"\x03\x00" + b"\x00" * 40
        def elf(data, _):
            machine = int.from_bytes(data[18:20], "little")
            return {"class": 32, "machine": machine, "attributes": [], "needed": ["libc.so"],
                    "imports": ["malloc"], "exports": ["JNI_OnLoad"], "jni_hints": ["JNI_OnLoad"],
                    "native_entry_points": []}
        with patch.object(p, "elf_report", side_effect=elf):
            source = self.apk("mixed.apk", more=[("lib/armeabi-v7a/liba.so", fake_arm),
                                                 ("lib/x86/liba.so", fake_x86)])
            result = p.analyze([source], "unused", "unused")
            self.assertEqual({x["kind"] for x in result["findings"]} & {"arm32", "multi_abi"},
                             {"arm32", "multi_abi"})
            self.assertEqual(result["files"][0]["libraries"][0]["needed"], ["libc.so"])
            x86 = self.apk("x86.apk", more=[("lib/x86/liba.so", fake_x86)])
            self.assertEqual(p.analyze([x86], "unused", "unused")["status"], "unsupported")
        malformed = self.apk("malformed.apk", more=[("lib/armeabi/liba.so", b"not ELF")])
        with self.assertRaisesRegex(p.Invalid, "malformed native ELF"):
            p.analyze([malformed], "unused", "unused")

    def test_install_and_runtime_gaps_are_separate(self):
        xml = (b'<manifest xmlns:android="http://schemas.android.com/apk/res/android" '
               b'package="test.fixture" android:versionCode="1" android:sharedUserId="legacy.uid">'
               b'<permission android:name="test.fixture.SIG" android:protectionLevel="signature"/>'
               b'<uses-feature android:name="android.hardware.vulkan.level" android:required="true"/>'
               b'<application><activity android:name="android.app.NativeActivity">'
               b'<meta-data android:name="android.app.lib_name" android:value="guest"/>'
               b'</activity></application></manifest>')
        result = p.analyze([self.apk("gaps.apk", xml)], "unused", "unused")
        self.assertEqual(result["status"], "unsupported")
        categories = {f["kind"]: f["category"] for f in result["findings"]}
        self.assertEqual(categories["shared_uid"], "install_signing")
        self.assertEqual(categories["signature_permission"], "install_signing")
        self.assertEqual(categories["native_activity"], "runtime")
        self.assertEqual(categories["declared_features"], "runtime")

    def test_guest_linkage_reports_missing_symbols_and_abi_mismatch(self):
        sysroot = self.root / "sysroot"
        (sysroot / "system/lib").mkdir(parents=True)
        (sysroot / "system/lib64").mkdir(parents=True)
        (sysroot / "system/lib/libplatform.so").write_bytes(b"platform")
        (sysroot / "system/lib64/libwrong.so").write_bytes(b"host")
        source = self.apk("libs.apk", more=[("lib/armeabi-v7a/libapp.so", b"app")])

        def elf(data, _):
            if data == b"app":
                return {"class": 32, "machine": 40, "needed": ["libplatform.so", "libwrong.so", "libabsent.so"],
                        "imports": ["present", "absent"], "weak_imports": ["optional"],
                        "exports": [], "versioned_imports": []}
            return {"class": 32, "machine": 40, "needed": [], "imports": [],
                    "exports": ["present"], "versioned_imports": []}

        with patch.object(p, "elf_report", side_effect=elf):
            report = p.analyze([source], "unused", "unused", sysroot)
            linkage = report["guest_linkage"]["per_abi"]["armeabi-v7a"][0]
            self.assertEqual(linkage["missing_libraries"], ["libabsent.so"])
            self.assertEqual(linkage["abi_mismatches"], ["libwrong.so"])
            self.assertEqual(linkage["unresolved_symbols"], [])  # incomplete closure
            kinds = {item["kind"] for item in report["findings"]}
            self.assertIn("missing_guest_library", kinds)
            self.assertIn("guest_abi_mismatch", kinds)
            (sysroot / "system/lib64/libwrong.so").unlink()
            (sysroot / "system/lib/libwrong.so").write_bytes(b"platform")
            (sysroot / "system/lib/libabsent.so").write_bytes(b"platform")
            report = p.analyze([source], "unused", "unused", sysroot)
            linkage = report["guest_linkage"]["per_abi"]["armeabi-v7a"][0]
            self.assertEqual(linkage["unresolved_symbols"], ["absent"])
            support = self.root / "support"
            support.mkdir()
            (support / "libplatform.so").write_bytes(b"platform")
            (sysroot / "system/lib/libplatform.so").unlink()
            report = p.analyze([source], "unused", "unused", sysroot, support)
            self.assertEqual(report["guest_linkage"]["per_abi"]["armeabi-v7a"][0]["missing_libraries"], [])


if __name__ == "__main__":
    unittest.main()
