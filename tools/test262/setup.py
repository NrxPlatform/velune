#!/usr/bin/env python3
"""Fetch the Test262 revision pinned by Velune."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys

REPOSITORY = "https://github.com/tc39/test262.git"


def log(message: str) -> None:
    print(f"[test262] {message}", flush=True)


def run(
    command: list[str],
    *,
    cwd: pathlib.Path | None = None,
    capture: bool = False,
) -> str:
    if capture:
        completed = subprocess.run(
            command,
            cwd=cwd,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    else:
        completed = subprocess.run(
            command,
            cwd=cwd,
            check=False,
            text=True,
        )

    if completed.returncode != 0:
        if capture:
            if completed.stdout:
                print(completed.stdout, end="", file=sys.stderr)
            if completed.stderr:
                print(completed.stderr, end="", file=sys.stderr)

        raise SystemExit(completed.returncode)

    if capture:
        return completed.stdout.strip()

    return ""


def main() -> int:
    script = pathlib.Path(__file__).resolve()
    root = script.parents[2]

    parser = argparse.ArgumentParser(
        description="Fetch Velune's pinned Test262 checkout"
    )
    parser.add_argument(
        "--destination",
        type=pathlib.Path,
        default=root / "third_party" / "test262",
        help="Test262 checkout directory",
    )
    parser.add_argument(
        "--revision-file",
        type=pathlib.Path,
        default=root / "tests" / "test262" / "TEST262_REVISION",
        help="file containing the pinned Test262 commit",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="replace an existing non-matching checkout",
    )

    args = parser.parse_args()

    revision = args.revision_file.read_text(
        encoding="utf-8"
    ).strip()

    if (
        len(revision) != 40
        or any(
            ch not in "0123456789abcdefABCDEF"
            for ch in revision
        )
    ):
        raise SystemExit(
            f"invalid Test262 revision in "
            f"{args.revision_file}: "
            "expected a full 40-character Git SHA"
        )

    destination = args.destination.resolve()
    destination.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    log(f"revision: {revision}")
    log(f"destination: {destination}")

    if destination.exists() and not (
        destination / ".git"
    ).exists():
        if not args.force:
            raise SystemExit(
                f"{destination} exists but is not a Git checkout; "
                "use --force to replace it"
            )

        log("removing non-Git destination")
        shutil.rmtree(destination)

    if not destination.exists():
        log("initializing Test262 repository")

        destination.mkdir(parents=True)

        run(
            ["git", "init"],
            cwd=destination,
        )

        run(
            [
                "git",
                "remote",
                "add",
                "origin",
                REPOSITORY,
            ],
            cwd=destination,
        )

    current_remote = run(
        ["git", "remote", "get-url", "origin"],
        cwd=destination,
        capture=True,
    )

    if current_remote != REPOSITORY:
        if not args.force:
            raise SystemExit(
                f"unexpected Test262 origin "
                f"{current_remote!r}; "
                "use --force to reset it"
            )

        log("resetting Test262 origin")

        run(
            [
                "git",
                "remote",
                "set-url",
                "origin",
                REPOSITORY,
            ],
            cwd=destination,
        )

    current_revision = ""

    try:
        current_revision = run(
            ["git", "rev-parse", "HEAD"],
            cwd=destination,
            capture=True,
        )
    except SystemExit:
        pass

    if current_revision.lower() != revision.lower():
        log("fetching pinned revision")

        run(
            [
                "git",
                "fetch",
                "--depth=1",
                "origin",
                revision,
            ],
            cwd=destination,
        )

        log("checking out pinned revision")

        run(
            [
                "git",
                "checkout",
                "--detach",
                "FETCH_HEAD",
            ],
            cwd=destination,
        )
    else:
        log("requested revision already checked out")

    actual = run(
        ["git", "rev-parse", "HEAD"],
        cwd=destination,
        capture=True,
    )

    if actual.lower() != revision.lower():
        raise SystemExit(
            f"Test262 revision mismatch: "
            f"expected {revision}, got {actual}"
        )

    log("validating checkout")

    required = [
        destination / "test",
        destination / "harness",
        destination / "INTERPRETING.md",
    ]

    missing = [
        str(path)
        for path in required
        if not path.exists()
    ]

    if missing:
        raise SystemExit(
            "incomplete Test262 checkout; missing: "
            + ", ".join(missing)
        )

    log("ready")
    print(f"Test262: {destination}")
    print(f"revision: {actual}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())