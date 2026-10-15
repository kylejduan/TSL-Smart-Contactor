"""Synthetic -fstack-usage files; no ESP-IDF build or device required."""
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import check_stack_budget as budget

C = '/COMPONENT_CONTROLLER_DIR/client.cpp'


def su(get_frame, fetch=True):
    lines = [
        '/COMPONENT_MAIN_DIR/tesla.cpp:21:6:void app::{anonymous}::worker(void*)\t10496\tstatic',
        '/COMPONENT_MAIN_DIR/ota.cpp:76:6:void app::{anonymous}::worker(void*)\t928\tstatic',
        f'{C}:32:7:tsl::Error tsl::FleetClient::refresh(const tsl::Config&)\t32\tstatic',
        f'{C}:32:7:tsl::Error tsl::FleetClient::_ZN3tsl11FleetClient7refreshERKNS_6ConfigE$part$0(const tsl::Config&)\t35648\tstatic',
        f'{C}:79:7:tsl::Error tsl::FleetClient::get(tsl::Endpoint, const tsl::Config&, tsl::Observation&)\t{get_frame}\tstatic',
        f'{C}:108:7:tsl::Error tsl::FleetClient::poll(const tsl::Config&, uint32_t, tsl::Observation&)\t80\tstatic',
    ]
    if fetch:
        lines.append(f'{C}:90:7:tsl::Error tsl::FleetClient::fetch(tsl::Endpoint, const tsl::Config&, tsl::Observation&)\t16672\tstatic')
    return '\n'.join(lines) + '\n'


class StackBudget(unittest.TestCase):
    def load(self, text):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'client.cpp.su').write_text(text)
            return budget.frames(Path(d))

    def test_outlined_parts_add_to_their_wrapper_and_workers_stay_distinct(self):
        found = self.load(su(48))
        self.assertEqual(found['tsl::FleetClient::refresh'], 35680)
        self.assertEqual(found[budget.WORKER], 10496)

    def test_split_recovery_fits_and_nested_response_frame_fails(self):
        worst = max(used for _, used, _ in budget.check(self.load(su(48)), 65536))
        self.assertEqual(worst, 10496 + 80 + 48 + 35680)
        self.assertGreaterEqual(65536 - worst, budget.RESERVE_BYTES)
        nested = max(used for _, used, _ in budget.check(self.load(su(16672)), 65536))
        self.assertLess(65536 - nested, budget.RESERVE_BYTES)

    def test_inlined_fetch_and_dynamic_frames_are_rejected(self):
        with self.assertRaises(SystemExit):
            budget.check(self.load(su(48, fetch=False)), 65536)
        with self.assertRaises(SystemExit):
            self.load(su(48).replace('16672\tstatic', '16672\tdynamic'))

    def test_stack_size_is_read_from_the_tesla_task(self):
        self.assertEqual(budget.stack_bytes(Path(budget.__file__).resolve().parents[1] / 'main/tesla.cpp'), 65536)


if __name__ == '__main__':
    unittest.main()
