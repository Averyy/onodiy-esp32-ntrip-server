# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Exercise the real diagnostic download against paginated API response sequences."""
import json
import shutil
import subprocess
from pathlib import Path

import pytest

UI = Path(__file__).resolve().parents[3] / "components/web/ui/js/82-network-diag.js"


def download(pages):
    if not shutil.which("node"):
        pytest.skip("Node.js unavailable")
    script = UI.read_text() + r"""
const pages = JSON.parse(process.argv[1]);
const cursors = [];
networkDiagDownload(async cursor => { cursors.push(cursor); return pages.shift(); },
                    async () => pages.shift()).then(
  data => console.log(JSON.stringify({data, cursors})),
  error => console.log(JSON.stringify({error: error.message, cursors})));
"""
    return json.loads(subprocess.check_output(["node", "-e", script, json.dumps(pages)], text=True))


def page(seqs, *, generation=1, last=35, active=False):
    return {"generation": generation, "active": active, "last_seq": last,
            "samples": [{"seq": s, "ping": [{"result": "timeout", "rtt_ms": None}]} for s in seqs],
            "transmit": [{"slot": 0, "tcp": {"available": False, "state": None}}]}


def test_download_keeps_loss_and_tcp_unavailability_across_pages():
    out = download([page(range(1, 33)), page(range(33, 36))])
    assert out["cursors"] == [32]
    assert [s["seq"] for s in out["data"]["samples"]] == list(range(1, 36))
    assert out["data"]["samples"][-1]["ping"][0]["rtt_ms"] is None
    assert out["data"]["transmit"][0]["tcp"]["state"] is None


@pytest.mark.parametrize("next_page", [page(range(33, 36), generation=2),
                                      page(range(33, 36), active=True), page([])])
def test_download_refuses_a_restarted_or_incomplete_recording(next_page):
    assert "error" in download([page(range(1, 33)), next_page])


def test_empty_recording_can_export_passive_sender_diagnostics():
    assert download([page([], last=0)])["data"]["samples"] == []


def test_download_preserves_tcp_sampling_cost_even_without_probe_rows():
    data = page([], last=0)
    cost = {"samples": 2, "total_us": 551, "max_us": 501, "last_us": 50,
            "histogram": [1, 0, 1, 0, 0, 0, 0, 0], "saturated": False}
    data["transmit"][0]["tcp_sampling"] = cost
    data["transmit"][0]["tcp"]["sample_duration_us"] = 50
    saved = download([data])["data"]["transmit"][0]
    assert saved["tcp_sampling"] == cost
    assert saved["tcp"]["sample_duration_us"] == 50
