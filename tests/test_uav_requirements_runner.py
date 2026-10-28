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

    def test_missing_candidate_type_is_not_proof_of_direct_transport(self):
        rows = self.valid()
        del rows[0]["pair"]["remoteCandidateType"]
        self.assertEqual(self.pairs(rows)["status"], "FAIL")

    def test_negative_transport_test_rejects_any_selected_pair(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "server.log"
            log.write_text("")
            self.assertEqual(protocol_evidence(log, "udp", "transport-reject")["status"], "PASS")
            log.write_text(json.dumps({"msg": "selected ICE candidate pair changed", **self.valid()[0]}))
            self.assertEqual(protocol_evidence(log, "udp", "transport-reject")["status"], "FAIL")

    def test_udp_media_failure_is_not_hidden_as_untested(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            report = {"started_at": "test", "scenarios": [{"protocol": "udp", "scenario": "video",
                "codec": "h264", "folder": "udp-video-h264", "ok": False, "error": "no decoded video"}]}
            finish(report, Path(directory))
            for rid in ("V-01", "V-02", "V-04", "V-13", "Q-05"):
                self.assertIn(rid, report["gaps"])

    def test_negative_failure_is_assigned_to_client_policy(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            report = {"started_at": "test", "scenarios": [{"protocol": "udp", "scenario": "transport-reject",
                "codec": "h264", "folder": "udp-transport-reject-h264", "ok": False}]}
            finish(report, Path(directory))
            self.assertIn("B-07", report["gaps"])
            self.assertNotIn("V-13", report["gaps"])

    def test_setup_failure_preserves_all_untested_requirements(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            report = {"started_at": "test", "setup_error": "build failed"}
            finish(report, Path(directory))
            self.assertEqual(len(REQUIREMENTS), 34)
            self.assertEqual(len(report["requirements"]), 34)
            self.assertTrue(all(r["status"] == "未测" for r in report["requirements"]))
            self.assertFalse(report["full_acceptance"])
            self.assertFalse(report["gaps"])

    def test_successful_smoke_does_not_erase_known_implementation_gaps(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            report = {"started_at": "test", "sha256": {"server": "test-build"}, "scenarios": []}
            finish(report, Path(directory))
            for rid in ("V-12", "Q-06", "Q-07", "Q-08", "P-11", "N-09"):
                self.assertIn(rid, report["source_assessment_gaps"])
                self.assertIn(rid, report["gaps"])
            self.assertFalse(report["full_acceptance"])

    def test_failed_h265_probe_cannot_be_counted_as_success(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            report = {"started_at": "test", "scenarios": [{"protocol": "udp", "scenario": "video",
                "codec": "h265", "folder": "udp-video-h265", "ok": False, "error": "Unsupported video codec"}]}
            finish(report, Path(directory))
            self.assertIn("V-12", report["gaps"])
            self.assertFalse(report["full_acceptance"])

    def test_failed_tcp_media_is_reported_as_a_gap(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            report = {"started_at": "test", "scenarios": [{"protocol": "tcp", "scenario": "video",
                "codec": "h264", "folder": "tcp-video-h264", "ok": False, "error": "WebRTC transport failed"}]}
            finish(report, Path(directory))
            self.assertIn("B-07", report["gaps"])
            row = next(r for r in report["requirements"] if r["id"] == "B-07")
            self.assertEqual(row["status"], "未通过")

    def test_dynacast_failure_is_a_gap_and_success_is_not_device_acceptance(self):
        for ok in (False, True):
            with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
                report = {"started_at": "test", "scenarios": [{"protocol": "udp", "scenario": "dynacast",
                    "codec": "h264", "folder": "udp-dynacast-h264", "ok": ok}]}
                finish(report, Path(directory))
                self.assertEqual("V-08" in report["gaps"], not ok)
                row = next(r for r in report["requirements"] if r["id"] == "V-08")
                self.assertIn("udp-dynacast-h264", row["evidence"])
                self.assertFalse(report["full_acceptance"])


if __name__ == "__main__":
    unittest.main()
