from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

from setuptools import setup
from setuptools.command.build_py import build_py as _build_py
from wheel.bdist_wheel import bdist_wheel as _bdist_wheel


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
        root = Path(__file__).resolve().parent.parent
        static_lib = root / "target" / "release" / "libblinkcms_datarouter.a"
        if not static_lib.exists():
            subprocess.run(
                ["cargo", "build", "--release"],
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
                "cc",
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


setup(cmdclass={"build_py": build_py, "bdist_wheel": bdist_wheel})
