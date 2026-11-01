import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
import wave

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "stem_worker.py"
spec = importlib.util.spec_from_file_location("stem_worker", SCRIPT)
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)
remote_spec = importlib.util.spec_from_file_location("remote_worker", ROOT / "scripts" / "remote_worker.py")
remote = importlib.util.module_from_spec(remote_spec)
remote_spec.loader.exec_module(remote)


class StemWorkerProtocolTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.cache = self.root / "cache"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.source = self.root / "source.wav"
        with wave.open(str(self.source), "wb") as stream:
            stream.setparams((2, 2, 44100, 0, "NONE", "not compressed"))
            stream.writeframes(b"\x01\x00\x02\x00" * 100)
        self.result = self.root / "result.json"
        self.status = self.root / "status.json"
        self.env = {**os.environ, "SLICELAB_STEM_CACHE": str(self.cache), "PATH": str(self.bin) + os.pathsep + os.environ["PATH"], "TEST_ROOT": str(self.root)}
        self.fake("ssh", "import sys\nprint('Permission denied', file=sys.stderr)\nsys.exit(255)\n")

    def tearDown(self):
        self.temp.cleanup()

    def fake(self, name, code):
        path = self.bin / name
        path.write_text("#!" + sys.executable + "\n" + code)
        path.chmod(0o755)

    def call(self):
        return subprocess.run([sys.executable, str(SCRIPT), "separate", "--input", str(self.source), "--result", str(self.result), "--status", str(self.status)], env=self.env, text=True, capture_output=True, timeout=20)

    def seed_cache(self):
        input_hash = worker.sha256(self.source)
        key = worker.result_key(input_hash)
        directory = self.cache / key
        directory.mkdir(parents=True)
        files = {}
        for stem in worker.STEMS:
            path = directory / (stem + ".wav")
            with wave.open(str(path), "wb") as stream:
                stream.setparams((2, 3, 44100, 0, "NONE", "not compressed"))
                stream.writeframes(b"\x01\x00\x00\x02\x00\x00" * 100)
            files[stem] = {"filename": path.name, "size": path.stat().st_size, "sha256": worker.sha256(path)}
        manifest = {"schema": 1, "worker_version": "1.2.0", "job_id": key, "input_sha256": input_hash, "model": worker.MODEL, "model_sha256": worker.MODEL_SHA256, "config_sha256": worker.CONFIG_SHA256, "sample_rate": 44100, "channels": 2, "frames": 100, "files": files, "runtime": worker.RUNTIME, "settings": worker.SETTINGS}
        (directory / "manifest.json").write_text(json.dumps(manifest))
        return directory, manifest

    def test_verified_cache_publishes_four_stems_without_network(self):
        directory, _ = self.seed_cache()
        completed = self.call()
        self.assertEqual(completed.returncode, 0, completed.stderr)
        result = json.loads(self.result.read_text())
        self.assertEqual(result["input_sha256"], worker.sha256(self.source))
        self.assertEqual((result["sample_rate"], result["channels"], result["frames"]), (44100, 2, 100))
        self.assertEqual(result["stems"], {stem: str(directory / (stem + ".wav")) for stem in worker.STEMS})
        self.assertTrue(json.loads(self.status.read_text())["cached"])

    def test_corrupt_cache_fails_with_login_instruction_and_removes_stale_result(self):
        directory, _ = self.seed_cache()
        (directory / "drums.wav").write_bytes(b"corrupt")
        self.result.write_text('{"stale": true}')
        completed = self.call()
        self.assertNotEqual(completed.returncode, 0)
        self.assertFalse(self.result.exists())
        status = json.loads(self.status.read_text())
        self.assertEqual(status["state"], "error")
        self.assertIn("ssh wojtek-ai-pc", status["detail"])
        self.assertIsNone(status["progress"])

    def test_cached_stem_with_wrong_frame_count_is_rejected_even_if_rehashed(self):
        directory, manifest = self.seed_cache()
        path = directory / "bass.wav"
        with wave.open(str(path), "wb") as stream:
            stream.setparams((2, 3, 44100, 0, "NONE", "not compressed"))
            stream.writeframes(b"\x00" * 6 * 99)
        manifest["files"]["bass"].update(size=path.stat().st_size, sha256=worker.sha256(path))
        (directory / "manifest.json").write_text(json.dumps(manifest))
        completed = self.call()
        self.assertNotEqual(completed.returncode, 0)
        self.assertEqual(json.loads(self.status.read_text())["state"], "error")
        self.assertFalse(self.result.exists())

    def test_missing_stem_or_wrong_model_never_uses_cache(self):
        directory, manifest = self.seed_cache()
        del manifest["files"]["vocals"]
        (directory / "manifest.json").write_text(json.dumps(manifest))
        self.assertNotEqual(self.call().returncode, 0)
        manifest["files"]["vocals"] = manifest["files"]["drums"]
        manifest["model_sha256"] = "0" * 64
        (directory / "manifest.json").write_text(json.dumps(manifest))
        self.assertNotEqual(self.call().returncode, 0)
        self.assertFalse(self.result.exists())

    def test_cancellation_stops_the_job_started_by_this_process(self):
        self.fake("scp", "pass\n")
        self.fake("ssh", '''import json, os, shlex, sys, time
args = shlex.split(sys.argv[-1])
command = next(x for x in ("prepare", "publish", "start", "status", "cancel") if x in args)
job = args[args.index("--job-id") + 1]
if command == "prepare": print(json.dumps({"path": "/tmp/slicelab-test/" + job, "state": "missing"}))
elif command == "publish": print(json.dumps({"state": "uploaded"}))
elif command == "start": print(json.dumps({"started": True, "state": "processing"}))
elif command == "status":
    open(os.path.join(os.environ["TEST_ROOT"], "polling"), "w").close()
    time.sleep(30)
elif command == "cancel":
    open(os.path.join(os.environ["TEST_ROOT"], "cancelled"), "w").close()
    print(json.dumps({"state": "cancelling"}))
''')
        process = subprocess.Popen([sys.executable, str(SCRIPT), "separate", "--input", str(self.source), "--result", str(self.result), "--status", str(self.status)], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 10
            while not (self.root / "polling").exists() and process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertTrue((self.root / "polling").exists())
            process.send_signal(signal.SIGTERM)
            _, stderr = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 130, stderr)
            self.assertTrue((self.root / "cancelled").exists())
            status = json.loads(self.status.read_text())
            self.assertEqual(status["state"], "cancelled")
            self.assertEqual(status["remote_state"], "cancelling")
            self.assertFalse(self.result.exists())
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()

    def test_remote_upload_rejects_corruption_before_replacing_input(self):
        job = self.root / ("a" * 64)
        job.mkdir()
        (job / "input.wav").write_bytes(b"previous input")
        (job / "input.upload").write_bytes(b"incomplete upload")
        with self.assertRaisesRegex(RuntimeError, "checksum"):
            remote.publish(job, "0" * 64)
        self.assertEqual((job / "input.wav").read_bytes(), b"previous input")
        uploaded_hash = worker.sha256(job / "input.upload")
        self.assertEqual(remote.publish(job, uploaded_hash)["state"], "uploaded")
        self.assertEqual((job / "input.wav").read_bytes(), b"incomplete upload")

    def test_remote_cache_checks_all_stems_before_reuse(self):
        directory, manifest = self.seed_cache()
        job = self.root / directory.name
        output = job / "output"
        output.mkdir(parents=True)
        for stem in worker.STEMS:
            (output / (stem + ".wav")).write_bytes((directory / (stem + ".wav")).read_bytes())
        (job / "manifest.json").write_text(json.dumps(manifest))
        self.assertTrue(remote.completed_result_valid(job, manifest["input_sha256"]))
        (output / "bass.wav").write_bytes(b"corrupt")
        self.assertFalse(remote.completed_result_valid(job, manifest["input_sha256"]))


if __name__ == "__main__":
    unittest.main()
