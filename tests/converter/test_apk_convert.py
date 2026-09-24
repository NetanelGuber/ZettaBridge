"""Converter admission checks for split mapping and host/guest boundaries."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import apk_convert as c
import apk_preflight as p


def item(split, libraries):
    return {"manifest": {"split": split}, "libraries": libraries}


def lib(abi, name):
    return {"abi": abi, "path": f"lib/{abi}/{name}"}


class LayoutTest(unittest.TestCase):
    def test_split_guest_mapping_uses_v7a_variant(self):
        report = {"status": "analyzed", "files": [
            item(None, [lib("armeabi", "liba.so")]),
            item("feature", [lib("armeabi-v7a", "liba.so"), lib("armeabi-v7a", "libb.so")])],
            "findings": []}
        base, mapping = c.source_layout(report, [(Path("base.apk"), "base"),
                                                 (Path("feature.apk"), "feature")])
        self.assertEqual(base, 0)
        self.assertEqual(mapping["liba.so"][1], 1)
        self.assertEqual(mapping["libb.so"][2]["path"], "lib/armeabi-v7a/libb.so")

    def test_duplicate_same_abi_and_host_abi_fail_closed(self):
        for files, message in (
            ([item(None, [lib("armeabi", "liba.so")]),
              item("feature", [lib("armeabi", "liba.so")])], "duplicate"),
            ([item(None, [lib("arm64-v8a", "liba.so")])], "ARM32-only"),
            ([item(None, [lib("armeabi", "libzbridge.so")])], "collides")):
            with self.subTest(message=message):
                report = {"status": "analyzed", "files": files, "findings": []}
                with self.assertRaisesRegex(p.Invalid, message):
                    c.source_layout(report, [(Path(str(i) + ".apk"), str(i))
                                             for i in range(len(files))])

    def test_unsupported_preflight_cannot_convert(self):
        report = {"status": "unsupported", "files": [item(None, [])],
                  "findings": [{"level": "unsupported", "kind": "shared_uid"}]}
        with self.assertRaisesRegex(p.Invalid, "shared_uid"):
            c.source_layout(report, [(Path("base.apk"), "base")])


if __name__ == "__main__":
    unittest.main()
