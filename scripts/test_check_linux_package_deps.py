#!/usr/bin/env python3
"""Behavior tests for the Linux package dependency guard.

The first case reproduces the bug that motivated the guard: the native video
plane added wayland-client, wayland-egl and egl to the runner's link line while
the hand-maintained depends lists went untouched.
"""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
CHECKER = ROOT / "scripts/check_linux_package_deps.py"
LINUX = ROOT / "linux"
# Every file the checker reads, relative to the linux/ directory it is given.
FIXTURE_FILES = (
    "runner/CMakeLists.txt",
    "CMakeLists.txt",
    "flutter/CMakeLists.txt",
    "packaging/build-packages.py",
    "packaging/bundle-libs.sh",
)


class LinuxPackageDepsGuardTest(unittest.TestCase):
    def _run(self, **edits: str) -> subprocess.CompletedProcess[str]:
        """Copy the real linux/ inputs, apply edits, and check the copy."""
        with tempfile.TemporaryDirectory(prefix="plezy-linux-deps-test-") as directory:
            linux = Path(directory) / "linux"
            for name in FIXTURE_FILES:
                target = linux / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(edits.get(name, self._source(name)), encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(CHECKER), str(linux)],
                cwd=ROOT,
                check=False,
                capture_output=True,
                text=True,
            )

    def _source(self, name: str) -> str:
        return (LINUX / name).read_text(encoding="utf-8")

    def _mutate(self, name: str, old: str, new: str) -> str:
        text = self._source(name).replace(old, new, 1)
        self.assertNotEqual(text, self._source(name), f"fixture mutation no longer matches: {old!r}")
        return text

    def test_current_tree_passes(self) -> None:
        result = self._run()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("dependency checks passed", result.stdout)

    def test_the_dependency_lists_before_the_video_plane_are_rejected(self) -> None:
        # Verbatim the deb list as it stood while the runner already linked
        # wayland-client, wayland-egl and egl: the shipped bug.
        packages = self._mutate(
            "packaging/build-packages.py",
            '"libwayland-client0",\n            "libwayland-egl1",\n            "libegl1",\n',
            "",
        )

        result = self._run(**{"packaging/build-packages.py": packages})

        self.assertEqual(result.returncode, 1)
        self.assertIn("wayland-client", result.stderr)
        self.assertIn("wayland-egl", result.stderr)
        self.assertIn("libegl1", result.stderr)
        self.assertIn("can fail to start", result.stderr)

    def test_one_missing_distro_is_rejected(self) -> None:
        # A dependency declared for deb but forgotten for rpm still ships broken
        # on Fedora, so per-distro coverage is the unit, not per-library.
        packages = self._mutate("packaging/build-packages.py", '"libglvnd-egl",\n', "")

        result = self._run(**{"packaging/build-packages.py": packages})

        self.assertEqual(result.returncode, 1)
        self.assertIn("rpm package does not depend on 'libglvnd-egl'", result.stderr)
        self.assertNotIn("deb package does not depend", result.stderr)

    def test_pacman_shared_wayland_package_counts_for_both_modules(self) -> None:
        # Arch has no separate libwayland-egl, so one entry has to satisfy two
        # modules. Dropping it must fail for both rather than neither.
        packages = self._mutate("packaging/build-packages.py", '"wayland",\n', "")

        result = self._run(**{"packaging/build-packages.py": packages})

        self.assertEqual(result.returncode, 1)
        self.assertIn("links wayland-client but the pacman package", result.stderr)
        self.assertIn("links wayland-egl but the pacman package", result.stderr)

    def test_new_pkgconfig_link_without_a_package_mapping_is_rejected(self) -> None:
        # The forward-looking half: the next library added to the runner has to
        # name its runtime package before it can ship.
        cmake = self._mutate(
            "runner/CMakeLists.txt",
            "target_link_libraries(${BINARY_NAME} PRIVATE PkgConfig::EGL)",
            "pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)\n"
            "target_link_libraries(${BINARY_NAME} PRIVATE PkgConfig::EGL)\n"
            "target_link_libraries(${BINARY_NAME} PRIVATE PkgConfig::PIPEWIRE)",
        )

        result = self._run(**{"runner/CMakeLists.txt": cmake})

        self.assertEqual(result.returncode, 1)
        self.assertIn("libpipewire-0.3", result.stderr)
        self.assertIn("RUNTIME_PACKAGES", result.stderr)

    def test_a_link_with_no_pkg_check_modules_is_rejected(self) -> None:
        cmake = self._mutate(
            "runner/CMakeLists.txt",
            "pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)",
            "# EGL declaration removed",
        )

        result = self._run(**{"runner/CMakeLists.txt": cmake})

        self.assertEqual(result.returncode, 1)
        self.assertIn("PkgConfig::EGL is linked into the runner", result.stderr)

    def test_bundling_the_excluded_libraries_is_rejected(self) -> None:
        # The guard's premise is that these come from the host. If bundle-libs.sh
        # starts shipping them, the demand for a depends entry needs rethinking
        # rather than silently continuing to hold.
        bundle = self._mutate("packaging/bundle-libs.sh", r"libwayland.*\.so|", "")

        result = self._run(**{"packaging/bundle-libs.sh": bundle})

        self.assertEqual(result.returncode, 1)
        self.assertIn("no longer excludes", result.stderr)

    def test_an_unlinked_module_is_not_demanded(self) -> None:
        # glib-2.0 and gio-2.0 are declared in flutter/CMakeLists.txt but are not
        # linked into the runner by name, so the guard must not require them.
        # It would otherwise grow into a list of everything pkg-config can see.
        result = self._run()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("(6 pkg-config links)", result.stdout)


if __name__ == "__main__":
    unittest.main()
