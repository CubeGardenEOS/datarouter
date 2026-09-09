from __future__ import annotations

import shutil
import os
import subprocess
import tarfile
import tempfile
from pathlib import Path

from setuptools import setup
from setuptools.command.build_py import build_py as _build_py
from setuptools.command.sdist import sdist as _sdist
from wheel.bdist_wheel import bdist_wheel as _bdist_wheel


def source_root() -> Path:
    project = Path(__file__).resolve().parent
    bundled = project / "_native_source"
    return bundled if bundled.is_dir() else project.parent


class sdist(_sdist):
    """Carry the exact Cargo source package so an sdist builds independently."""
    def make_release_tree(self, base_dir, files):
        super().make_release_tree(base_dir, files)
        root = source_root()
        destination = Path(base_dir) / "_native_source"
        if root.name == "_native_source":
            shutil.copytree(root, destination, ignore=shutil.ignore_patterns("target"))
            return
        (root / "target").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="python-source-", dir=root / "target") as target:
            subprocess.run(["cargo", "package", "--allow-dirty", "--locked", "--no-verify",
                            "--target-dir", target], cwd=root, check=True)
            packages = list((Path(target) / "package").glob("blinkcms-datarouter-*.crate"))
            if len(packages) != 1:
                raise RuntimeError("expected exactly one current Cargo source package")
            with tarfile.open(packages[0]) as archive:
                for member in archive:
                    relative = Path(*Path(member.name).parts[1:])
                    if not member.isfile() or relative.is_absolute() or ".." in relative.parts:
                        raise RuntimeError("invalid Cargo source archive member")
                    path = destination / relative
                    path.parent.mkdir(parents=True, exist_ok=True)
                    with archive.extractfile(member) as source, path.open("wb") as output:
                        shutil.copyfileobj(source, output)


class bdist_wheel(_bdist_wheel):
    """Force a platform-specific wheel tag.

    The wheel ships a native `_native/libblinkcms_datarouter.so`, so it must
    not be tagged `py3-none-any` (which would let pip install it on the wrong
    platform or share it across unrelated caches).
    """

    def finalize_options(self) -> None:
        super().finalize_options()
        self.root_is_pure = False

    def get_tag(self) -> tuple[str, str, str]:
        # The shipped .so is a plain C/Rust library loaded via ctypes: it has
        # no CPython C-API dependency, so tag it python-version independent
        # (py3-none) while keeping the platform tag.
        _, _, plat = super().get_tag()
        return "py3", "none", plat


class build_py(_build_py):
    def run(self) -> None:
        self._build_datarouter_so()
        super().run()

    def _build_datarouter_so(self) -> None:
        root = source_root()
        static_lib = root / "target" / "release" / "libblinkcms_datarouter.a"
        subprocess.run(
            ["cargo", "build", "--release", "--locked", "--target-dir", str(root / "target")],
            cwd=str(root),
            check=True,
        )
        if not static_lib.exists():
            raise RuntimeError(f"static library not found: {static_lib}")

        so_name = "libblinkcms_datarouter.so"
        package_dir = Path(self.build_lib) / "blinkcms_datarouter" / "_native"
        package_dir.mkdir(parents=True, exist_ok=True)
        so_path = package_dir / so_name

        subprocess.run(
            [
                os.environ.get("CC") or "cc",
                "-shared",
                "-o",
                str(so_path),
                "-Wl,--whole-archive",
                str(static_lib),
                "-Wl,--no-whole-archive",
                "-lrt",
                "-lpthread",
                "-ldl",
            ],
            check=True,
        )


setup(cmdclass={"build_py": build_py, "bdist_wheel": bdist_wheel, "sdist": sdist})
