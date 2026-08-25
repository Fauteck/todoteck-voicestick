#!/usr/bin/env python3
"""Schreibt `firmware.json` neben die beiden Abbilder.

Aufgerufen aus `.github/workflows/firmware.yml` mit `dist/` als
Arbeitsverzeichnis. Eigene Datei statt eines Heredocs im Workflow, weil das
Zusammenspiel aus YAML-Blockskalar, Shell-Quoting und JSON-Klammern dort
schlecht zu lesen und noch schlechter zu aendern ist.

Das Ergebnis ist der Grund, warum die Abholung auf dem Buero-PC guenstig ist:
sie laedt zuerst diese knapp 400 Byte, vergleicht `commit` mit dem, was zuletzt
abgelegt wurde, und spart sich bei Gleichstand die zwei Megabyte. Die SHA-256
je Datei erlaubt ihr ausserdem zu pruefen, ob der Download vollstaendig war,
bevor sie die alte Fassung ueberschreibt.
"""

from __future__ import annotations

import hashlib
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

FILES = ("voice_stick.bin", "merged.bin")


def describe(name: str) -> dict[str, object]:
    data = Path(name).read_bytes()
    if not data:
        raise SystemExit(f"{name} ist leer — das kann kein gueltiges Abbild sein.")
    return {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def main() -> int:
    sha = os.environ.get("GITHUB_SHA", "")
    manifest = {
        "commit": sha,
        "commit_short": sha[:7],
        "run_id": os.environ.get("GITHUB_RUN_ID", ""),
        "built_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "files": {name: describe(name) for name in FILES},
    }
    Path("firmware.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(json.dumps(manifest, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
