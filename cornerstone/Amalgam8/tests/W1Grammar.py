"""Build and run an Amalgam8 module grammar fixture in both output modes."""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
from tempfile import TemporaryDirectory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("amalgam8", type=Path)
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    compiler = args.amalgam8.resolve(strict=True)
    cc = shlex.split(os.environ.get("CC", "cc"))
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    fixtures = Path(__file__).resolve().with_name("W1Fixtures")
    with TemporaryDirectory(dir=args.temp_dir) as temporary:
        work = Path(temporary)
        source = work / "fixture"
        shutil.copytree(fixtures, source)
        root = source / "root.am8"
        subprocess.run([compiler, "--api", root], check=True, cwd=work)
        consumer = source / "consumer.c"
        consumer.write_text('#include "root.hpp"\nint main(void) { return answer() != 42; }\n',
                            encoding="utf-8")
        api_program = work / "api"
        subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", consumer, "-o", api_program],
                       check=True, cwd=work)
        subprocess.run([api_program], check=True, cwd=work)
        subprocess.run([compiler, "--cli", root], check=True, cwd=work)
        cli_program = work / "cli"
        subprocess.run([*cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", root.with_suffix(".cpp"),
                        "-o", cli_program], check=True, cwd=work)
        subprocess.run([cli_program], check=True, cwd=work)


if __name__ == "__main__":
    main()
