"""Guard against false-positive protocol/acceptance reports (no server needed)."""
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

from uav_requirements import finish, protocol_evidence, REQUIREMENTS


class EvidenceTests(unittest.TestCase):
    def pairs(self, rows, protocol="tcp"):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "server.log"
            log.write_text("\n".join(json.dumps({"msg": "selected ICE candidate pair changed", **r}) for r in rows))
            return protocol_evidence(log, protocol, "video")

    def row(self, participant, transport, local="tcp", remote="tcp", kind="host"):
        return {"participant": participant, "transport": transport,
                "pair": {"localProtocol": local, "remoteProtocol": remote,
                         "localCandidateType": kind, "remoteCandidateType": "host"}}

    def valid(self):
        return [self.row("drone", "PUBLISHER"), self.row("viewer-a", "SUBSCRIBER"),
                self.row("viewer-b", "SUBSCRIBER")]

    def test_both_media_legs_and_every_viewer_required(self):
        self.assertEqual(self.pairs(self.valid())["status"], "PASS")
        self.assertEqual(self.pairs(self.valid()[:2])["status"], "FAIL")
        self.assertEqual(self.pairs([])["status"], "FAIL")

    def test_wrong_protocol_and_relay_are_rejected(self):
        rows = self.valid()
        rows[0] = self.row("drone", "PUBLISHER", remote="udp")
        self.assertEqual(self.pairs(rows)["status"], "FAIL")
        rows[0] = self.row("drone", "PUBLISHER", kind="relay")
        self.assertEqual(self.pairs(rows)["status"], "FAIL")

    def test_setup_failure_preserves_all_untested_requirements(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            report = {"started_at": "test", "setup_error": "build failed"}
            finish(report, Path(directory))
            self.assertEqual(len(REQUIREMENTS), 34)
            self.assertEqual(len(report["requirements"]), 34)
            self.assertTrue(all(r["status"] == "未测" for r in report["requirements"]))
            self.assertFalse(report["full_acceptance"])
            self.assertFalse(report["gaps"])

    def test_failed_h265_probe_cannot_be_counted_as_success(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            report = {"started_at": "test", "scenarios": [{"protocol": "udp", "scenario": "video",
                "codec": "h265", "folder": "udp-video-h265", "ok": False, "error": "Unsupported video codec"}]}
            finish(report, Path(directory))
            self.assertIn("V-12", report["gaps"])
            self.assertFalse(report["full_acceptance"])


if __name__ == "__main__":
    unittest.main()
