import json
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
REQUIRED = {
    "schema",
    "device_time_us",
    "algo_tick",
    "emit_tick",
    "seq",
    "algo_id",
    "algo_enabled",
    "values",
}


class ProtocolExampleTest(unittest.TestCase):
    def test_example_contains_required_fields(self):
        lines = (ROOT / "examples" / "serial-frame.ndjson").read_text(encoding="utf-8-sig").splitlines()
        self.assertTrue(lines)
        for line in lines:
            frame = json.loads(line)
            self.assertFalse(REQUIRED - frame.keys())
            self.assertEqual(frame["schema"], "akrion.frame/1")
            self.assertIsInstance(frame["device_time_us"], int)
            self.assertIsInstance(frame["algo_id"], int)
            self.assertIsInstance(frame["algo_enabled"], bool)
            self.assertIsInstance(frame["values"], dict)
            self.assertTrue(all(isinstance(value, (int, float)) for value in frame["values"].values()))
            self.assertGreaterEqual(frame["device_time_us"], 0)

    def test_schema_and_example_agree_on_required_fields(self):
        schema = json.loads((ROOT / "schemas" / "serial-frame.schema.json").read_text(encoding="utf-8-sig"))
        self.assertEqual(set(schema["required"]), REQUIRED)

    def test_run_schema_describes_canonical_directory(self):
        schema = json.loads((ROOT / "schemas" / "run.schema.json").read_text(encoding="utf-8-sig"))
        self.assertEqual(schema["properties"]["frames"]["items"]["$ref"], "serial-frame.schema.json")
        self.assertEqual(
            set(schema["x-canonical-files"]),
            {"manifest.json", "config.json", "serial.raw", "frames.ndjson", "events.ndjson", "summary.json"},
        )

    def test_config_and_manifest_use_v1_schemas(self):
        config = json.loads((ROOT / "schemas" / "config.schema.json").read_text(encoding="utf-8-sig"))
        manifest = json.loads((ROOT / "schemas" / "manifest.schema.json").read_text(encoding="utf-8-sig"))
        self.assertEqual(config["properties"]["schema"]["const"], "akrion.config/1")
        self.assertEqual(manifest["properties"]["schema"]["const"], "akrion.manifest/1")


if __name__ == "__main__":
    unittest.main()
