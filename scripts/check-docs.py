#!/usr/bin/env python3
"""Prueft aufzaehlbare Doku-Aussagen gegen den Code.

Doku veraltet an drei Stellen, und alle drei sind Aussagen, die nichts
nachrechnet (siehe `Doku-Hygiene` in CLAUDE.md). Dieses Skript nimmt der
dritten Sorte -- der handgepflegten Aufzaehlung -- die Handpflege ab: Wo eine
Menge aus dem Code auch in der Doku steht, wird sie hier verglichen.

Geprueft werden nur Mengen, die sich aus dem Code ableiten lassen. Prosa,
Beispiele und Begruendungen stehen bewusst nicht unter Vertrag.

Die geprueften Stellen sind in den Markdown-Dateien markiert:

    <!-- doku-vertrag:name --> ... <!-- /doku-vertrag -->

Die Marke haelt den Vertrag an der Aussage fest, nicht an der Formulierung:
Text drumherum darf umgeschrieben werden, ohne dass hier etwas bricht.

Aufruf: python3 scripts/check-docs.py   (Rueckgabe 0 = alles deckt sich)
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

VOICE_BLE_C = ROOT / "firmware/components/voice_ble/voice_ble.c"
MAIN_C = ROOT / "firmware/main/main.c"
UI_STATUS_C = ROOT / "firmware/components/ui_status/ui_status.c"
BOARD_H = ROOT / "firmware/components/stick_s3_board/include/stick_s3_board.h"
VERSION_TXT = ROOT / "firmware/version.txt"
README = ROOT / "README.md"
PROTOCOL = ROOT / "docs/protocol.md"

failures: list[str] = []


def fail(vertrag: str, doc: Path, message: str) -> None:
    failures.append(f"{doc.relative_to(ROOT)} [{vertrag}]: {message}")


def region(doc: Path, name: str) -> str:
    """Der Text zwischen den Marken. Fehlt die Marke, ist das ein Fehler --
    sonst verschwaende ein geloeschter Kommentar den Vertrag lautlos."""
    text = doc.read_text(encoding="utf-8")
    match = re.search(
        r"<!--\s*doku-vertrag:" + re.escape(name) + r"\s*-->(.*?)<!--\s*/doku-vertrag\s*-->",
        text,
        re.S,
    )
    if not match:
        raise LookupError(f"{doc.relative_to(ROOT)}: Marke `doku-vertrag:{name}` fehlt")
    return match.group(1)


def c_strings(source: str) -> str:
    """Aneinandergehaengte C-Zeichenketten zu ihrem Inhalt aufloesen."""
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', source)
    return "".join(parts).replace('\\"', '"')


def compare(vertrag: str, doc: Path, was: str, doku, code) -> None:
    if isinstance(doku, list):
        doku, code = list(doku), list(code)
    if doku == code:
        return
    if isinstance(doku, list):
        nur_doku = [x for x in doku if x not in code]
        nur_code = [x for x in code if x not in doku]
        detail = []
        if nur_doku:
            detail.append(f"steht in der Doku, nicht im Code: {nur_doku}")
        if nur_code:
            detail.append(f"steht im Code, nicht in der Doku: {nur_code}")
        if not detail:
            detail.append(f"andere Reihenfolge: Doku {doku}, Code {code}")
        fail(vertrag, doc, f"{was} -- " + "; ".join(detail))
    else:
        fail(vertrag, doc, f"{was} -- Doku sagt {doku!r}, Code sagt {code!r}")


# --- GATT-Merkmale: docs/protocol.md gegen voice_ble.c ----------------------
#
# Die UUIDs stehen im Code als 16 Bytes in umgekehrter Reihenfolge; verglichen
# wird die Menge, nicht die Zeile. Der Dienst selbst wird getrennt geprueft.

def uuids_from_code() -> dict[str, str]:
    source = VOICE_BLE_C.read_text(encoding="utf-8")
    found = {}
    for name, body in re.findall(
        r"static const ble_uuid128_t (s_\w+)\s*=\s*BLE_UUID128_INIT\(([^)]*)\)", source
    ):
        raw = [b.strip() for b in body.split(",") if b.strip()]
        digits = "".join(f"{int(b, 16):02x}" for b in reversed(raw))
        found[name] = (
            f"{digits[0:8]}-{digits[8:12]}-{digits[12:16]}-{digits[16:20]}-{digits[20:32]}"
        )
    return found


def check_gatt() -> None:
    code = uuids_from_code()
    if not code:
        fail("gatt", PROTOCOL, "keine UUID im Code gefunden -- Parser oder Code kaputt")
        return

    service_code = code.pop("s_service_uuid", None)
    service_doc = re.findall(r"[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}",
                             region(PROTOCOL, "gatt-dienst"))
    compare("gatt-dienst", PROTOCOL, "Dienst-UUID", service_doc, [service_code])

    doc = re.findall(r"[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}",
                     region(PROTOCOL, "gatt-merkmale"))
    compare("gatt-merkmale", PROTOCOL, "Merkmals-UUIDs", sorted(doc), sorted(code.values()))


# --- device_info: docs/protocol.md gegen voice_ble.c und main.c -------------

def check_device_info() -> None:
    source = VOICE_BLE_C.read_text(encoding="utf-8")
    body = re.search(r"voice_ble_send_device_info\(void\)\s*\{(.*?)\n\}", source, re.S)
    if not body:
        fail("device-info", PROTOCOL, "voice_ble_send_device_info() nicht gefunden")
        return
    json_code = c_strings(body.group(1))

    doc = region(PROTOCOL, "device-info")

    for feld in ("hardware", "buttons", "interaction_modes", "ui_states"):
        code_wert = re.search(rf'"{feld}":(\[[^\]]*\]|"[^"]*")', json_code)
        doku_wert = re.search(rf'"{feld}":(\[[^\]]*\]|"[^"]*")', doc)
        if not code_wert or not doku_wert:
            fail("device-info", PROTOCOL, f"Feld `{feld}` fehlt in Doku oder Code")
            continue
        compare("device-info", PROTOCOL, f"`{feld}`", doku_wert.group(1), code_wert.group(1))

    # Die Firmware-Fassung kommt aus esp_app_get_description(), also aus
    # firmware/version.txt. Ein Beispiel mit einer erfundenen Nummer laedt dazu
    # ein, sie fuer die aktuelle zu halten.
    version_doc = re.search(r'"firmware_version":"([^"]*)"', doc)
    version_code = VERSION_TXT.read_text(encoding="utf-8").strip()
    if not version_doc:
        fail("device-info", PROTOCOL, "`firmware_version` fehlt im Beispiel")
    else:
        compare("device-info", PROTOCOL, "`firmware_version`",
                version_doc.group(1), version_code)

    # Angekuendigte Zustaende muessen auch angenommen werden.
    announced = re.findall(r'"([a-z_]+)"', re.search(r'"ui_states":\[([^\]]*)\]', json_code).group(1))
    handled = re.findall(r'strcmp\(state, "([a-z_]+)"\) == 0', MAIN_C.read_text(encoding="utf-8"))
    fehlend = [s for s in announced if s not in handled]
    if fehlend:
        failures.append(
            f"firmware/main/main.c [device-info]: device_info kuendigt `ui_state`-Zustaende an, "
            f"die main.c nicht annimmt: {fehlend}"
        )


# --- Tastenrollen: docs/protocol.md gegen stick_s3_board.h ------------------

def check_button_roles() -> None:
    """Welche Taste spricht und welche blaettert.

    Die Zuordnung hat 08/2026 gewechselt, und sie steht an zwei Stellen: als
    Makro im Board-Header und als Tabelle im Protokoll. Genau die Sorte
    Aussage, die beim naechsten Wechsel an einer der beiden Stellen stehen
    bleibt -- die Doku behauptet dann eine Belegung, die das Geraet nicht hat,
    und niemand merkt es, weil das Protokoll ja nur Rollen kennt.
    """
    source = BOARD_H.read_text(encoding="utf-8")

    pins = dict(re.findall(r"#define\s+STICK_S3_PIN_BUTTON_(FRONT|SIDE)\s+(\d+)", source))
    rollen = dict(re.findall(
        r"#define\s+STICK_S3_PIN_BUTTON_(TALK|BROWSE)\s+STICK_S3_PIN_BUTTON_(FRONT|SIDE)",
        source))
    if len(pins) != 2 or len(rollen) != 2:
        fail("tasten-rollen", PROTOCOL, "Tastenbelegung in stick_s3_board.h nicht lesbar")
        return

    # Rolle im Protokoll <- Rolle im Code: sprechen ist `primary`.
    code = {
        "primary": (rollen["TALK"].lower(), pins[rollen["TALK"]]),
        "secondary": (rollen["BROWSE"].lower(), pins[rollen["BROWSE"]]),
    }

    doc = {}
    for zeile in region(PROTOCOL, "tasten-rollen").splitlines():
        treffer = re.match(r"\|\s*`(primary|secondary)`\s*\|\s*(front|side)\b[^|]*\|\s*(\d+)\s*\|",
                           zeile.strip())
        if treffer:
            doc[treffer.group(1)] = (treffer.group(2), treffer.group(3))

    compare("tasten-rollen", PROTOCOL, "Tastenrollen", doc, code)


# --- Schriftstufen: README gegen ui_status.c -------------------------------

def check_font_steps() -> None:
    source = UI_STATUS_C.read_text(encoding="utf-8")
    block = re.search(r"TEXT_STEPS\[\]\s*=\s*\{(.*?)\};", source, re.S)
    if not block:
        fail("schriftstufen", README, "TEXT_STEPS nicht gefunden")
        return
    code = [int(n) for n in re.findall(r"&todoteck_font_(\d+)", block.group(1))]
    doc = [int(n) for n in re.findall(r"\d+", region(README, "schriftstufen"))]
    compare("schriftstufen", README, "Schriftstufen fuer den Antworttext",
            sorted(doc), sorted(code))


# --- Abhaengigkeiten: README gegen die idf_component.yml -------------------

def check_dependencies() -> None:
    code = []
    for yml in sorted(ROOT.glob("firmware/**/idf_component.yml")):
        text = yml.read_text(encoding="utf-8")
        block = re.search(r"^dependencies:\s*$(.*?)(?=^\S|\Z)", text, re.S | re.M)
        if not block:
            continue
        code += re.findall(r"^\s{2}([\w/\-]+):", block.group(1), re.M)
    doc = re.findall(r"`([\w/\-]+)`", region(README, "firmware-abhaengigkeiten"))
    compare("firmware-abhaengigkeiten", README, "Komponenten-Abhaengigkeiten",
            sorted(doc), sorted(code))


def main() -> int:
    checks = (check_gatt, check_device_info, check_button_roles, check_font_steps,
              check_dependencies)
    for check in checks:
        try:
            check()
        except LookupError as exc:
            failures.append(str(exc))

    if failures:
        print("Doku und Code gehen auseinander:\n", file=sys.stderr)
        for line in failures:
            print(f"  - {line}", file=sys.stderr)
        print(
            "\nDie Doku wird an den Code angeglichen, nicht umgekehrt -- "
            "es sei denn, der Code ist der Fehler.",
            file=sys.stderr,
        )
        return 1

    print(f"Doku und Code decken sich ({len(checks)} Pruefungen).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
