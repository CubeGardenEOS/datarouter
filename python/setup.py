from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

from setuptools import setup
from setuptools.command.build_py import build_py as _build_py


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


setup(cmdclass={"build_py": build_py})
