import json
import subprocess
import sys

exe = sys.argv[1]
valid = subprocess.run([exe, '--json'], check=True, capture_output=True, text=True)
report = json.loads(valid.stdout)
assert report['matching_build'] is True
assert report['records'][0]['ndis_status_raw'] == '0xc0230015'
assert report['records'][0]['lso_info_raw'] == '0xfedcba9876543210'
assert report['records'][0]['reason_name'] == 'gso-plan'
assert report['events_are_unique_nbls'] is False
for case in ['--bad-count', '--bad-source', '--bad-sequence', '--bad-reason']:
    result = subprocess.run([exe, case], capture_output=True)
    assert result.returncode == 1 and not result.stdout, case
different = subprocess.run([exe, '--different-build'], capture_output=True, text=True)
assert different.returncode == 3 and json.loads(different.stdout)['matching_build'] is False
print('PASS: actual send formatter preserves raw values and rejects malformed snapshots before JSON output.')

pressure = subprocess.run([exe, '--pressure-json'], check=True, capture_output=True, text=True)
report = json.loads(pressure.stdout)
assert report['matching_build'] is True and report['sample_stride'] == 64
assert report['extrapolation'] is False and report['snapshot_atomic'] is False
entry = report['records'][0]
assert entry['ndis_status_raw'] == '0xc000009a' and entry['counts_valid'] is True
assert (entry['ready_unread_slots'], entry['user_held_or_late_return_slots'], entry['returned_unrecycled_slots']) == (2, 1, 1)
assert entry['lso_info_raw'] == '0xfedcba9876543210'
assert report['current_queues'][0]['free'] == 1 and report['current_queues'][0]['published'] == 3
for case in ['count', 'source', 'sequence', 'ledger', 'cursor', 'flags', 'current-count', 'current-slots']:
    result = subprocess.run([exe, '--pressure-bad-' + case], capture_output=True)
    assert result.returncode == 1 and not result.stdout, case
unknown = subprocess.run([exe, '--pressure-invalid-observation'], check=True, capture_output=True, text=True)
report = json.loads(unknown.stdout)
assert report['records'][0]['counts_valid'] is False and report['sample_contains']['unknown'] == 1
different = subprocess.run([exe, '--pressure-different-build'], capture_output=True, text=True)
assert different.returncode == 3 and json.loads(different.stdout)['matching_build'] is False
print('PASS: actual pressure formatter preserves raw status and slot stages, reports invalid observations as unknown, and rejects malformed snapshots before output.')
