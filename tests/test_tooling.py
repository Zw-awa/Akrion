import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class QtToolingTest(unittest.TestCase):
    def test_agent_safe_build_template_is_present(self):
        helper = (ROOT / "cmake" / "QtAgentSafe.cmake").read_text(encoding="utf-8-sig")
        script = (ROOT / "tools" / "qt.ps1").read_text(encoding="utf-8-sig")
        self.assertIn("qt_agent_add_qml_executable", helper)
        self.assertIn("qt_wrap_cpp", helper)
        self.assertIn("CODEX_THREAD_ID", script)
        self.assertIn("MinGW Makefiles", script)
        self.assertIn("Read-DotEnv", script)
        self.assertNotIn("F:\\Qt", script)
        self.assertTrue((ROOT / ".env.example").exists())
        self.assertIn(".env", (ROOT / ".gitignore").read_text(encoding="utf-8-sig"))


if __name__ == "__main__":
    unittest.main()

