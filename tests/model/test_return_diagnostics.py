"""Decode the actual probe formatter output; never opens a driver or adapter."""
import json
import subprocess
import sys
import unittest

EXECUTABLE = sys.argv.pop(1)


class ReturnDiagnostics(unittest.TestCase):
    def run_case(self, mode):
        return subprocess.run([EXECUTABLE, mode], capture_output=True, text=True, timeout=10)

    def test_raw_positive_negative_and_local_status_are_distinct(self):
        result = self.run_case("--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["tool_version"], "2.0.0")
        self.assertEqual(report["ndis_nonzero_nbls"], 2)
        self.assertEqual(report["records"][0]["nbl_status_raw"], "0x00010003")
        self.assertEqual(report["records"][1]["nbl_status_raw"], "0xc0000001")
        self.assertTrue(report["records"][0]["ndis_status_observed"])
        local = report["records"][2]
        self.assertEqual(local["nbl_status_raw"], "0xc023002a")
        self.assertEqual(local["origin"], "local-cancel-before-indication")
        self.assertFalse(local["ndis_status_observed"])
        self.assertEqual(local["completion_status"], 7)
        self.assertEqual(local["ip_declared_length"], 64)
        self.assertEqual(local["classification"], "local-cancel-before-indication")
        self.assertEqual(local["completion_status_name"], "PB_STOPPED")
        self.assertEqual(report["records"][0]["nbl_status_name"], "UNCLASSIFIED")
        self.assertEqual(report["records"][0]["classification"], "unclassified-nonzero-return")

    def test_observed_icmp_rejections_keep_raw_status_and_packet_scope(self):
        result = self.run_case("--protocol-rejected-json")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["completion_scope"], "rx-packet-return")
        self.assertEqual(report["ndis_nonzero_nbls"], 2)
        self.assertEqual(report["local_cancelled_packets"], 0)
        self.assertEqual([(r["ip_version"], r["ip_protocol"], r["ip_length"])
                          for r in report["records"]], [(4, 1, 576), (6, 58, 1280)])
        for record in report["records"]:
            self.assertEqual(record["nbl_status_raw"], "0xc000023e")
            self.assertEqual(record["nbl_status_name"], "STATUS_PROTOCOL_UNREACHABLE")
            self.assertEqual(record["classification"], "protocol-unreachable")
            self.assertEqual(record["completion_status"], 4)
            self.assertEqual(record["completion_status_name"], "PB_RX_REJECTED")
            self.assertTrue(record["ndis_status_observed"])

    def test_malformed_output_is_rejected_before_emitting_json(self):
        for mode in ["--bad-count", "--bad-source", "--bad-sequence", "--unknown-origin", "--bad-record"]:
            with self.subTest(mode=mode):
                result = self.run_case(mode)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, "")

    def test_read_only_build_mismatch_is_visible(self):
        result = self.run_case("--different-build")
        self.assertEqual(result.returncode, 3)
        self.assertFalse(json.loads(result.stdout)["matching_build"])


if __name__ == "__main__":
    unittest.main()
