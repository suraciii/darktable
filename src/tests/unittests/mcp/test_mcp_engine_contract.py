#!/usr/bin/env python3
"""Black-box MCP contract scenario against an explicitly selected binary."""
import base64
import copy
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import unittest
import tifffile

try:
    from PIL import Image
except ImportError as exc:
    raise SystemExit(f"Pillow is required for the native MCP scenario: {exc}")


class McpClient:
    def __init__(self, binary, config, read_only=False):
        self.proc = subprocess.Popen(
            [binary, *(["--read-only"] if read_only else []), "--core", "--disable-opencl",
             "--configdir", str(config), "--cachedir", str(config / "cache"),
             "--library", str(config / "library.db"),
             "--conf", "plugins/darkroom/workflow=none",
             "--conf", "write_sidecar_files=never",
             "--conf", "plugins/imageio/format/tiff/bpp=32",
             "--conf", "plugins/imageio/format/tiff/compress=1"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, bufsize=1,
        )
        self.next_id = 1
        self.call("initialize", {})
        self.notify("notifications/initialized", {})

    def notify(self, method, params):
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method, "params": params}) + "\n")
        self.proc.stdin.flush()

    def call(self, method, params):
        request = {"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params}
        self.next_id += 1
        self.proc.stdin.write(json.dumps(request) + "\n")
        self.proc.stdin.flush()
        ready, _, _ = select.select([self.proc.stdout], [], [], 180)
        if not ready:
            raise AssertionError("timed out waiting for MCP response")
        line = self.proc.stdout.readline()
        self.assert_alive(line)
        response = json.loads(line)
        if "error" in response:
            raise AssertionError(response["error"])
        return response["result"]

    def tool(self, name, arguments):
        result = self.call("tools/call", {"name": name, "arguments": arguments})
        if result.get("isError"):
            text = "\n".join(item.get("text", "") for item in result.get("content", []))
            raise RuntimeError(text)
        content = result.get("content", [])
        text = next(item["text"] for item in content if item.get("type") == "text")
        return json.loads(text)

    def tool_error(self, name, arguments):
        result = self.call("tools/call", {"name": name, "arguments": arguments})
        if not result.get("isError"):
            raise AssertionError(f"{name} unexpectedly succeeded: {result}")
        return "\n".join(item.get("text", "") for item in result.get("content", []))

    def image(self, arguments):
        result = self.call("tools/call", {"name": "render", "arguments": arguments})
        if result.get("isError"):
            raise AssertionError(result)
        item = next(item for item in result.get("content", []) if item.get("type") == "image")
        data = base64.b64decode(item["data"])
        self.assert_png(data)
        return data

    @staticmethod
    def assert_png(data):
        if data[:8] != b"\x89PNG\r\n\x1a\n" or len(data) <= 32:
            raise AssertionError("render did not return a non-empty PNG")

    def close(self):
        if self.proc.poll() is None:
            try:
                self.call("shutdown", {})
            except (AssertionError, BrokenPipeError):
                pass
            self.proc.terminate()
            self.proc.wait(timeout=15)
        self.proc.stdin.close()
        self.proc.stdout.close()

    def assert_alive(self, line):
        if not line:
            raise AssertionError("MCP exited without a JSON response")


def fingerprint(path):
    info = path.stat()
    return (hashlib.sha256(path.read_bytes()).digest(), info.st_size, info.st_mtime_ns, info.st_mode)

class EngineContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="darktable-mcp-contract-")
        root = Path(self.temp.name)
        self.config = root / "config"
        self.config.mkdir()
        self.source = root / "synthetic.tiff"
        image = Image.new("RGB", (32, 24))
        image.putdata([(x * 7, y * 9, (x + y) * 4) for y in range(24) for x in range(32)])
        image.save(self.source, format="TIFF")
        self.source_before = fingerprint(self.source)
        self.binary = Path(os.environ.get("DARKTABLE_MCP_BINARY", sys.argv[1]))
        self.icc = os.environ.get("ICC_PROPHOTO_LINEAR_PATH")
        if not self.icc:
            raise AssertionError("ICC_PROPHOTO_LINEAR_PATH must identify the qualified linear ProPhoto asset")
        self.assertEqual(hashlib.sha256(Path(self.icc).read_bytes()).hexdigest(),
                         "df7b2c677645f1ca5364b52e62f8db04ca61f80163792942f3e409a84a6b12ed")
        profile = self.config / "color/out/linear-prophoto.icc"
        profile.parent.mkdir(parents=True)
        shutil.copyfile(self.icc, profile)
        self.icc = str(profile)
        self.client = McpClient(self.binary, self.config)

    def tearDown(self):
        self.client.close()
        self.assertEqual(self.source_before, fingerprint(self.source))
        self.temp.cleanup()

    def import_source(self):
        result = self.client.tool("import_images", {"paths": [str(self.source)]})
        self.assertEqual(result["imported"], 1)
        return result["images"][0]["imgid"]

    def snapshot(self):
        tables = ("images", "history", "masks_history", "module_order", "history_hash",
                  "tagged_images", "color_labels", "meta_data", "film_rolls")
        with sqlite3.connect(f"file:{self.config / 'library.db'}?mode=ro", uri=True) as database:
            return {table: sorted(database.execute(f'SELECT * FROM "{table}"').fetchall(), key=repr)
                    for table in tables}

    def export(self, image_id, target, stack):
        return self.client.tool("export_images", {
            "input": {"imgid": image_id}, "out_path": str(target), "format": "scene-linear-tiff",
            "bpp": 32, "icc_file": self.icc, "stack": stack,
        })

    def test_recursive_codec_and_validation(self):
        modules = self.client.tool("list_modules", {})
        operations = [module["operation"] for module in modules if module.get("have_introspection")]
        schemas = {op: self.client.tool("module_schema", {"operation": op}) for op in operations}
        for operation in ("rgbcurve", "lens", "exposure"):
            version = schemas[operation]["params_version"]
            encoded = self.client.tool("encode_params", {"operation": operation, "fields": {}})
            decoded = self.client.tool("decode_params", {
                "operation": operation, "blob_hex": encoded["blob_hex"], "params_version": version,
            })
            round_trip = self.client.tool("encode_params", {
                "operation": operation, "fields": decoded["fields"],
            })
            self.assertEqual(round_trip["blob_hex"], encoded["blob_hex"])
            self.client.tool_error("decode_params", {
                "operation": operation, "blob_hex": encoded["blob_hex"], "params_version": version + 1,
            })
        fields = self.client.tool("decode_params", {
            "operation": "rgbcurve", "blob_hex": self.client.tool("encode_params", {
                "operation": "rgbcurve", "fields": {},
            })["blob_hex"], "params_version": schemas["rgbcurve"]["params_version"],
        })["fields"]
        self.assertEqual(len(fields["curve_nodes"]), 3)
        self.assertEqual(set(fields["curve_nodes"][0][0]), {"x", "y"})
        for fields in ({"mode": 999}, {"mode": "unknown"}, {"exposure": None},
                       {"exposure": "bad"}, {"unknown": 1}, {"deflicker_target_level": 1e38}):
            self.client.tool_error("encode_params", {"operation": "exposure", "fields": fields})

    def test_failed_requests_preserve_undeveloped_catalog(self):
        image_id = self.import_source()
        before = self.snapshot()
        self.client.tool("image_parameters", {"input": {"imgid": image_id}})
        self.assertEqual(before, self.snapshot())
        valid = {"operation": "exposure", "params": {"exposure": 1.0}, "multi_priority": 1}
        version = self.client.tool("module_schema", {"operation": "exposure"})["params_version"]
        blob = self.client.tool("encode_params", {"operation": "exposure", "fields": {}})["blob_hex"]
        invalid = [None, {}, [1], [{"operation": "exposure", "params": []}],
                   [valid, {"operation": "not-real", "params": {}}],
                   [valid, {"operation": "exposure", "params": {"exposure": "bad"}}],
                   [{"operation": "rgbcurve", "params": {"curve_num_nodes": [1.5, 2, 2]}}],
                   [{"operation": "exposure", "params": {}, "before": "rawprepare"}],
                   [{"operation": "exposure", "params": {}, "multi_priority": 1.5}],
                   [{"operation": "exposure", "blob_hex": blob, "params_version": version + 1}],
                   [{"operation": "exposure", "blob_hex": blob}],
                   [{"operation": "exposure", "params": {"mode": 999}}]]
        for stack in invalid:
            with self.subTest(stack=stack):
                self.client.tool_error("render", {"input": {"imgid": image_id}, "stack": stack})
                self.assertEqual(before, self.snapshot())
                target = self.config / "rejected.tiff"
                self.client.tool_error("export_images", {
                    "input": {"imgid": image_id}, "stack": stack,
                    "out_path": str(target), "format": "scene-linear-tiff", "icc_file": self.icc,
                })
                self.assertFalse(target.exists())
                self.assertEqual(before, self.snapshot())
        target = self.config / "directory.tiff"
        target.mkdir()
        self.client.tool_error("export_images", {
            "input": {"imgid": image_id}, "stack": [valid], "out_path": str(target),
            "format": "scene-linear-tiff", "icc_file": self.icc,
        })
        self.assertEqual(before, self.snapshot())
        self.assertEqual(list(target.iterdir()), [])
        self.assertEqual(list(self.config.glob("*.mcp-*")), [])

    def test_missing_or_unregistered_profile_refuses_without_catalog_edits(self):
        image_id = self.import_source()
        before = self.snapshot()
        unregistered = self.config / "unregistered.icc"
        shutil.copyfile(self.icc, unregistered)
        for profile in (None, str(self.config / "missing.icc"), str(unregistered)):
            target = self.config / "wrong-profile.tiff"
            self.client.tool_error("export_images", {
                "input": {"imgid": image_id}, "out_path": str(target),
                "format": "scene-linear-tiff", "icc_file": profile,
                "stack": [{"operation": "exposure", "params": {"exposure": 1.0}}],
            })
            self.assertFalse(target.exists())
            self.assertEqual(before, self.snapshot())

    def test_export_before_render_and_generic_instances(self):
        image_id = self.import_source()
        parameters = self.client.tool("image_parameters", {"input": {"imgid": image_id}})
        modules = {module["operation"]: module for module in parameters["modules"]}
        curve = copy.deepcopy(modules["rgbcurve"]["values"]["curve_nodes"])
        curve[0][1]["y"] = 0.75
        stack = [{"operation": "exposure", "params": {"exposure": 1.0}, "multi_priority": 1},
                 {"operation": "rgbcurve", "params": {"curve_nodes": curve}, "enabled": False},
                 {"operation": "lens", "params": {"lens": "MCP UTF-8 café"}, "enabled": False}]
        output = self.config / "export-first.tiff"
        self.export(image_id, output, stack)
        with tifffile.TiffFile(output) as tiff:
            samples = tiff.asarray()
            self.assertEqual(samples.shape, (24, 32, 3))
            self.assertEqual(samples.dtype, "float32")
            self.assertEqual(hashlib.sha256(tiff.pages[0].tags[34675].value).hexdigest(),
                             "7bef28a81c974482756f09c7d34c55d53549ba450f26185b2c16f6228af96dfe")
        after = self.client.tool("image_parameters", {"input": {"imgid": image_id}})
        exposures = [module for module in after["modules"] if module["operation"] == "exposure"]
        self.assertEqual({module["multi_priority"] for module in exposures}, {0, 1})
        second = next(module for module in exposures if module["multi_priority"] == 1)
        self.assertEqual(second["values"]["exposure"], 1.0)
        for field, value in second["defaults"].items():
            if field != "exposure":
                self.assertEqual(second["values"][field], value)
        result = {module["operation"]: module for module in after["modules"]}
        self.assertEqual(result["rgbcurve"]["values"]["curve_nodes"], curve)
        self.assertEqual(result["lens"]["values"]["lens"], "MCP UTF-8 café")
        self.client.image({"input": {"imgid": image_id}, "stack": stack, "width": 32, "height": 24})
        repeat = self.config / "after-render.tiff"
        self.export(image_id, repeat, stack)
        with tifffile.TiffFile(repeat) as tiff:
            self.assertTrue((samples == tiff.asarray()).all())
        self.assertFalse(self.source.with_suffix(".tiff.xmp").exists())

    def test_ambient_sidecars_are_ignored(self):
        xmp = b'''<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"><rdf:Description xmlns:xmp="http://ns.adobe.com/xap/1.0/" xmp:Rating="5" xmlns:darktable="http://darktable.sf.net/" darktable:xmp_version="5" /></rdf:RDF></x:xmpmeta>'''
        sidecars = [self.source.with_suffix(".tiff.xmp"), self.source.with_name("synthetic_01.tiff.xmp")]
        for sidecar in sidecars:
            sidecar.write_bytes(xmp)
        before = [fingerprint(sidecar) for sidecar in sidecars]
        image_id = self.import_source()
        images = self.client.tool("list_images", {})
        self.assertEqual([image["imgid"] for image in images], [image_id])
        self.assertNotEqual(images[0]["rating"], 5)
        self.export(image_id, self.config / "sidecar.tiff", [])
        self.assertEqual(before, [fingerprint(sidecar) for sidecar in sidecars])

    def test_export_refuses_original_and_aliases(self):
        image_id = self.import_source()
        before = self.snapshot()
        link = self.source.with_name("alias.tiff")
        link.symlink_to(self.source)
        hardlink = self.source.with_name("hardlink.tiff")
        os.link(self.source, hardlink)
        sidecar = self.source.with_suffix(".tiff.xmp")
        sidecar.write_bytes(b"external metadata")
        original = sidecar.read_bytes()
        for target in (self.source, link, hardlink, sidecar):
            self.client.tool_error("export_images", {
                "input": {"imgid": image_id}, "out_path": str(target), "format": "tiff",
                "stack": [{"operation": "exposure", "params": {"exposure": 1.0}}],
            })
            self.assertEqual(before, self.snapshot())
            self.assertEqual(self.source_before, fingerprint(self.source))
            self.assertEqual(original, sidecar.read_bytes())


if __name__ == "__main__":
    if len(sys.argv) < 2 and not os.environ.get("DARKTABLE_MCP_BINARY"):
        raise SystemExit("usage: test_mcp_engine_contract.py /path/to/darktable-mcp")
    unittest.main(argv=[sys.argv[0]])
