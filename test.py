from __future__ import annotations

import platform
import shutil
import subprocess
import unittest
from pathlib import Path

from dataclasses import dataclass

THIS_DIR = Path(__file__).parent.resolve()
TEST_BUILD_DIR_NAME = "tests-zmk-feature-watchdog"


def run_west(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["west", *args],
        capture_output=True,
        text=True,
        cwd=THIS_DIR,
    )


@dataclass
class NotFound:
    text: str


@dataclass
class ConfigAndDeviceTree:
    # Expected rows in .config
    config: list[str | NotFound]
    # Expected rows in devicetree_generated.h
    device: list[str | NotFound]


class WestCommandsTests(unittest.TestCase):
    WEST_TOPDIR: Path
    BUILD_DIR: Path

    @classmethod
    def setUpClass(cls):
        cls.WEST_TOPDIR = Path(run_west(["topdir"]).stdout.strip())
        cls.BUILD_DIR = cls.WEST_TOPDIR / "build"

    @unittest.skipUnless(
        platform.system() == "Linux", "zmk-test is only supported on Linux"
    )
    def test_zmk_test(self):
        test_build_dir = self.BUILD_DIR / TEST_BUILD_DIR_NAME
        shutil.rmtree(test_build_dir, ignore_errors=True)

        result = run_west(["zmk-test", "tests", "-m", ".", "-d", str(test_build_dir)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS: test", result.stdout, result.stdout + result.stderr)
        self.assertIn("PASS: studio", result.stdout, result.stdout + result.stderr)
        self.assertIn("PASS: watchdog", result.stdout, result.stdout + result.stderr)
        self.assertNotIn("FAILED: ", result.stdout, result.stdout + result.stderr)

    def test_zmk_build(self):
        self._test_zmk_build(
            {
                "module_watchdog_board_feature_disabled": ConfigAndDeviceTree(
                    config=[
                        'CONFIG_ZMK_KEYBOARD_NAME="Module Test"',
                        "CONFIG_ZMK_USB=y",
                        "CONFIG_ZMK_BLE=y",
                        "# CONFIG_ZMK_WATCHDOG is not set",
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_zmk_keymap",
                    ],
                ),
                "module_watchdog_board_with_rpc": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_WATCHDOG=y",
                        "CONFIG_ZMK_WATCHDOG_STUDIO_RPC=y",
                        # Detectors (Phase C): default y whenever ZMK_WATCHDOG
                        # is on, on a real (non-native_sim) board.
                        "CONFIG_ZMK_WATCHDOG_FREEZE_DETECT=y",
                        "CONFIG_ZMK_WATCHDOG_FATAL_DETECT=y",
                        "CONFIG_TASK_WDT=y",
                        # CONFIG_TASK_WDT_HW_FALLBACK defaults to y upstream
                        # whenever CONFIG_TASK_WDT=y, regardless of our own
                        # (removed) Kconfig -- but src/watchdog_freeze.c
                        # always calls task_wdt_init(NULL), so the hardware
                        # watchdog code in subsys/task_wdt/task_wdt.c (all
                        # gated behind `if (hw_wdt)`) never actually runs.
                        # Not asserting its absence here since it's an inert
                        # upstream default, not something this module
                        # controls or that indicates a regression.
                        # DESIGN.md SS7.2: the Studio RPC TX path never needs
                        # to be raised above its framework default.
                        "CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE=64",
                    ],
                    device=[],
                ),
                "module_watchdog_board_without_rpc": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_WATCHDOG=y",
                        "# CONFIG_ZMK_STUDIO is not set",
                        NotFound("CONFIG_ZMK_WATCHDOG_STUDIO_RPC"),
                        "CONFIG_ZMK_WATCHDOG_FREEZE_DETECT=y",
                        "CONFIG_ZMK_WATCHDOG_FATAL_DETECT=y",
                        # Test injection is dangerous and must default off,
                        # even on a build that otherwise enables every other
                        # detector.
                        NotFound("CONFIG_ZMK_WATCHDOG_TEST_INJECTION=y"),
                    ],
                    device=[],
                ),
                # Dedicated test-injection build (DESIGN.md SS4.4/SS12): the
                # only artifact with CONFIG_ZMK_WATCHDOG_TEST_INJECTION=y,
                # proving the InjectTest RPC wiring
                # (src/studio/watchdog_request_exec.c) compiles for a real
                # board, not just native_sim.
                "module_watchdog_board_test_injection": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_WATCHDOG=y",
                        "CONFIG_ZMK_WATCHDOG_STUDIO_RPC=y",
                        "CONFIG_ZMK_WATCHDOG_FREEZE_DETECT=y",
                        "CONFIG_ZMK_WATCHDOG_FATAL_DETECT=y",
                        "CONFIG_ZMK_WATCHDOG_TEST_INJECTION=y",
                        # Boot-delay auto-trigger Kconfig options exist but
                        # default to 0 (disabled) even on this artifact -- see
                        # the build.yaml comment for why they aren't baked
                        # into a standing build-test artifact. (Int Kconfigs
                        # always render with their value in .config, never
                        # "is not set" -- unlike bool/tristate.)
                        "CONFIG_ZMK_WATCHDOG_TEST_INJECT_FREEZE_AT_BOOT_MS=0",
                        "CONFIG_ZMK_WATCHDOG_TEST_INJECT_FATAL_AT_BOOT_MS=0",
                    ],
                    device=[],
                ),
                "custom_settings_board": ConfigAndDeviceTree(
                    config=[
                        # Verify that zmk-feature-custom-settings is present and enabled
                        "zmk-feature-custom-settings",
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_WATCHDOG=y",
                        "CONFIG_ZMK_WATCHDOG_STUDIO_RPC=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y",
                        "CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128",
                        "CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048",
                    ],
                    device=[],
                ),
                # Split keyboard build coverage (DESIGN.md SS7). Peripheral
                # role: no Studio (ZMK_STUDIO only selects ZMK_STUDIO_RPC for
                # !ZMK_SPLIT || ZMK_SPLIT_ROLE_CENTRAL), but the relay bridge
                # (peripheral responder side) + watchdog core still compile
                # and CONFIG_ZMK_WATCHDOG_PROTOBUF pulls in nanopb on its own.
                "module_watchdog_split_peripheral": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_WATCHDOG=y",
                        "CONFIG_ZMK_SPLIT=y",
                        NotFound("CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y"),
                        "CONFIG_ZMK_SPLIT_RELAY_EVENT=y",
                        "CONFIG_ZMK_WATCHDOG_SPLIT_RELAY=y",
                        "CONFIG_ZMK_WATCHDOG_PROTOBUF=y",
                        NotFound("CONFIG_ZMK_STUDIO=y"),
                        NotFound("CONFIG_ZMK_WATCHDOG_STUDIO_RPC=y"),
                        # DESIGN.md SS7.1: the split relay event payload never
                        # needs to be raised above its framework default.
                        "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN=128",
                    ],
                    device=[],
                ),
                # Central role: local Studio RPC + the relay bridge dispatch
                # side.
                "module_watchdog_split_central": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_WATCHDOG=y",
                        "CONFIG_ZMK_SPLIT=y",
                        "CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y",
                        "CONFIG_ZMK_SPLIT_RELAY_EVENT=y",
                        "CONFIG_ZMK_WATCHDOG_SPLIT_RELAY=y",
                        "CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST=y",
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_WATCHDOG_STUDIO_RPC=y",
                        # DESIGN.md SS7.1/SS7.2: neither buffer needs to be
                        # raised above its framework default any more, so
                        # this artifact's snippets no longer set them --
                        # verify they stay at the framework defaults.
                        "CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE=64",
                        "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN=128",
                    ],
                    device=[],
                ),
            }
        )

    def _test_zmk_build(
        self, artifacts_and_expected_build_params: dict[str, ConfigAndDeviceTree]
    ):

        for artifact in artifacts_and_expected_build_params.keys():
            shutil.rmtree(self.BUILD_DIR / artifact, ignore_errors=True)

        result = run_west(["zmk-build", "tests/zmk-config", "-q"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        for artifact, entries in artifacts_and_expected_build_params.items():
            artifact_dir = self.BUILD_DIR / artifact / "zephyr"
            config_path = artifact_dir / ".config"
            device_tree_path = (
                artifact_dir
                / "include"
                / "generated"
                / "zephyr"
                / "devicetree_generated.h"
            )
            self._test_strings_in_file(
                config_path, entries.config, f"{artifact} config"
            )
            if entries.device:
                self._test_strings_in_file(
                    device_tree_path, entries.device, f"{artifact} device tree"
                )
            self.assertTrue(
                (artifact_dir / "zmk.uf2").exists(),
                f"{artifact} zmk.uf2 is missing in {artifact_dir}",
            )

    def _test_strings_in_file(
        self, file_path: Path, expected_strings: list[str | NotFound], hint: str
    ):
        self.assertTrue(file_path.exists(), f"{hint}: {file_path} is missing")
        file_text = file_path.read_text()

        for expected in expected_strings:
            if isinstance(expected, NotFound):
                if expected.text in file_text:
                    self.fail(
                        f"{hint}: {expected.text} found in {file_path}, but it should not be present"
                    )
            else:
                if expected not in file_text:
                    self.fail(f"{hint}: {expected} not found in {file_path}")


if __name__ == "__main__":
    unittest.main()
