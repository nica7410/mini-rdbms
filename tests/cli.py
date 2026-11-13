"""Black-box CLI tests. Only Python's standard library is required."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

EXE = str(pathlib.Path(sys.argv.pop(1)).resolve())


class CliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mini-rdbms-cli-")
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.schemas = self.root / "schemas with spaces"
        self.schemas.mkdir()
        (self.schemas / "users.schema").write_text("id INT\nname TEXT\nage INT\n", encoding="utf-8")
        self.base = [EXE, "--schema-dir", str(self.schemas), "--data-dir", str(self.root / "data")]

    def run_cli(self, *args, stdin=None):
        return subprocess.run(self.base + list(args), input=stdin, capture_output=True, encoding="utf-8", timeout=15)

    def test_sql_file_and_process_restart(self):
        sql = self.root / "input with spaces.sql"
        sql.write_text("INSERT INTO users VALUES (1,'정글',20),(2,'O''Brien',30);\nSELECT name FROM users WHERE id=1;", encoding="utf-8-sig")
        result = self.run_cli("--file", str(sql))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "INSERT 2\nname\n정글\n")
        result = self.run_cli("--execute", "SELECT * FROM users WHERE id=2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "id,name,age\n2,O'Brien,30\n")

    def test_stdin(self):
        result = self.run_cli(stdin="INSERT INTO users VALUES (1,'A',10); SELECT id FROM users;")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "INSERT 1\nid\n1\n")

    def test_parse_error_does_not_execute_prior_statement(self):
        result = self.run_cli("--execute", "INSERT INTO users VALUES (1,'A',10); SELECT @ FROM users;")
        self.assertEqual(result.returncode, 1)
        self.assertIn("SQL 1:", result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertFalse((self.root / "data/users.mdb").exists())

    def test_runtime_error_keeps_prior_successful_statement(self):
        result = self.run_cli("--execute", "INSERT INTO users VALUES (1,'A',10); SELECT missing FROM users; INSERT INTO users VALUES (2,'B',20);")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "INSERT 1\n")
        self.assertEqual(self.run_cli("--execute", "SELECT id FROM users").stdout, "id\n1\n")

    def test_invalid_arguments_and_missing_file(self):
        for args in [("--unknown",), ("--file",), ("--file", "missing.sql"), ("--execute", "", "--file", "x")]:
            with self.subTest(args=args):
                result = self.run_cli(*args)
                self.assertEqual(result.returncode, 1)
                self.assertTrue(result.stderr.startswith("error:"))

    def test_help_and_empty_script(self):
        self.assertEqual(self.run_cli("--help").returncode, 0)
        result = self.run_cli("--execute", "-- comment only")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")

    def test_shipped_demo(self):
        project = pathlib.Path(__file__).resolve().parent.parent
        result = subprocess.run([EXE, "--schema-dir", str(project / "schemas"), "--data-dir", str(self.root / "demo"),
                                 "--file", str(project / "examples/demo.sql")], capture_output=True, encoding="utf-8", timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("김정글", result.stdout)
        self.assertIn("키보드,120000", result.stdout)


if __name__ == "__main__":
    unittest.main()
