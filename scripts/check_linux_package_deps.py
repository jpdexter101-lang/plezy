#!/usr/bin/env python3
"""Guard the Linux package dependency lists against what the runner links.

linux/packaging/bundle-libs.sh deliberately refuses to bundle the display- and
driver-coupled libraries (libEGL, libwayland-*, libGL, libdrm ...): they must
come from the host or the app will not talk to the compositor it is running
under. That makes them the package manager's problem, and the depends lists in
linux/packaging/build-packages.py are maintained by hand.

Nothing connected the two. Adding a pkg-config link to the runner produced a
binary with an undeclared shared-library dependency, and the failure surfaces
only on a user's machine at exec time - a class of bug no compile or unit test
can reach. This walks the actual link line instead:

  target_link_libraries(${BINARY_NAME} PRIVATE PkgConfig::WAYLAND_EGL)
    -> pkg_check_modules(WAYLAND_EGL REQUIRED IMPORTED_TARGET wayland-egl)
    -> RUNTIME_PACKAGES["wayland-egl"] -> libwayland-egl1 / libwayland-egl / wayland

and requires every distro to declare it. A new pkg-config module fails here
until its runtime package names are named for all three.
"""

from pathlib import Path
import ast
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
if len(sys.argv) > 2:
    raise SystemExit(f"Usage: {Path(sys.argv[0]).name} [linux-dir]")
LINUX = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else ROOT / "linux"

RUNNER_CMAKE = LINUX / "runner/CMakeLists.txt"
PACKAGES_PY = LINUX / "packaging/build-packages.py"
BUNDLE_SH = LINUX / "packaging/bundle-libs.sh"
# pkg_check_modules for targets the runner links may live in either file.
CMAKE_FILES = (RUNNER_CMAKE, LINUX / "CMakeLists.txt", LINUX / "flutter/CMakeLists.txt")

# pkg-config module -> the package that ships its runtime library, per distro.
# Only modules the runner actually links are consulted, so an unused entry here
# is harmless; a missing one is an error.
RUNTIME_PACKAGES = {
    "gtk+-3.0": {"deb": "libgtk-3-0", "rpm": "gtk3", "pacman": "gtk3"},
    "mpv": {"deb": "libmpv2 | libmpv1", "rpm": "mpv-libs", "pacman": "mpv"},
    "epoxy": {"deb": "libepoxy0", "rpm": "libepoxy", "pacman": "libepoxy"},
    "wayland-client": {
        "deb": "libwayland-client0",
        "rpm": "libwayland-client",
        # Arch ships every libwayland-* in the one `wayland` package.
        "pacman": "wayland",
    },
    "wayland-egl": {
        "deb": "libwayland-egl1",
        "rpm": "libwayland-egl",
        "pacman": "wayland",
    },
    # libglvnd is the vendor-neutral dispatch that provides libEGL.so.1.
    "egl": {"deb": "libegl1", "rpm": "libglvnd-egl", "pacman": "libglvnd"},
}

errors: list[str] = []


def require(condition: bool, message: str) -> None:
    if not condition:
        errors.append(message)


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as error:
        errors.append(f"{path}: cannot read: {error}")
        return ""


def linked_pkgconfig_targets(text: str) -> set[str]:
    """PkgConfig:: targets linked into the runner executable."""
    targets: set[str] = set()
    for match in re.finditer(
        r"target_link_libraries\(\s*\$\{BINARY_NAME\}[^)]*?PkgConfig::(\w+)", text
    ):
        targets.add(match.group(1))
    return targets


def pkgconfig_modules() -> dict[str, str]:
    """CMake variable prefix -> pkg-config module name."""
    modules: dict[str, str] = {}
    for path in CMAKE_FILES:
        if not path.is_file():
            continue
        for match in re.finditer(
            r"pkg_check_modules\(\s*(\w+)\b[^)]*?IMPORTED_TARGET\s+([\w.+-]+)", read(path)
        ):
            modules.setdefault(match.group(1), match.group(2))
    return modules


def declared_depends() -> dict[str, list[str]]:
    """distro -> depends list, read from the DISTROS literal by AST."""
    tree = ast.parse(read(PACKAGES_PY), filename=str(PACKAGES_PY))
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        if not any(isinstance(t, ast.Name) and t.id == "DISTROS" for t in node.targets):
            continue
        table = ast.literal_eval(node.value)
        return {name: list(config.get("depends", [])) for name, config in table.items()}
    errors.append(f"{PACKAGES_PY}: no DISTROS assignment to read the depends lists from")
    return {}


runner = read(RUNNER_CMAKE)
modules = pkgconfig_modules()
depends = declared_depends()

require(bool(depends), "no distro depends lists were found, so nothing was checked")

# The exclusion list is what makes declaring these mandatory rather than
# optional. If bundling ever starts covering them, this guard is the wrong shape.
bundle = read(BUNDLE_SH)
for pattern in (r"libEGL\.so", r"libwayland.*\.so"):
    require(
        pattern in bundle,
        f"bundle-libs.sh no longer excludes {pattern}: if those are bundled now, "
        "the depends entries this guard demands may be wrong",
    )

linked = linked_pkgconfig_targets(runner)
require(
    bool(linked),
    f"{RUNNER_CMAKE.name}: found no PkgConfig:: link on ${{BINARY_NAME}}; "
    "the link-line parse is broken, not the build",
)

for target in sorted(linked):
    module = modules.get(target)
    if module is None:
        errors.append(
            f"PkgConfig::{target} is linked into the runner but no pkg_check_modules "
            f"declares it in {', '.join(p.name for p in CMAKE_FILES)}"
        )
        continue
    packages = RUNTIME_PACKAGES.get(module)
    if packages is None:
        errors.append(
            f"pkg-config module '{module}' (PkgConfig::{target}) is linked into the runner "
            f"but has no entry in RUNTIME_PACKAGES: name the package that ships its "
            f"runtime library on each distro, then declare it in {PACKAGES_PY.name}"
        )
        continue
    for distro, declared in sorted(depends.items()):
        package = packages.get(distro)
        if package is None:
            errors.append(
                f"RUNTIME_PACKAGES['{module}'] has no '{distro}' package name, so the "
                f"{distro} package cannot declare a library the runner links"
            )
            continue
        require(
            package in declared,
            f"the runner links {module} but the {distro} package does not depend on "
            f"'{package}'; bundle-libs.sh will not bundle it, so an installed package "
            f"can fail to start",
        )

if errors:
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)
    sys.exit(1)

print(f"linux package dependency checks passed ({len(linked)} pkg-config links)")
