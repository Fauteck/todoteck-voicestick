# CLAUDE.md

Arbeitsregeln für dieses Repo. Was das Repo selbst beantwortet — Aufbau des
Forks, Protokoll, Release-Weg — steht in [README.md](README.md) und
[docs/](docs/) und wird hier nicht nacherzählt.

## Wissensquelle: llm-wiki (in Todoteck)

Projektübergreifendes Wissen, Konzepte und Entscheidungen stehen im Todoteck-Projekt
`llm-wiki` (Zugriff über den Todoteck-MCP, `search`/`get_note`). Einstieg für dieses
Repo ist die Übersichtsnotiz `todoteck-voicestick`.

<!-- heimat-regel v1 -->
**Heimat-Regel (gilt für jedes Fauteck-Repo, entschieden 2026-10-07).** Wissen lebt im Todoteck-Wiki `llm-wiki`. Im Repo liegt **nur**, was im selben PR wie der Code geändert oder von einem Guard oder Test geprüft wird: README, `CLAUDE.md`, Architektur-, Muster- und Konventions-Doku, API-Vertrag, Schema, Setup, Checklisten, Mechanik der Guards und Jobs. **Konzepte, Entscheidungen, Phasenverläufe, Befund-Berichte und Wissen über fremde Dienste gehören ins Wiki** — nicht in `docs/`, nicht als Notiz ins Projekt Home Lab. **Todoteck-Inhalte außerhalb von `llm-wiki` sind keine Wissensquelle** — Aufgaben, Unteraufgaben und Notizen in anderen Projekten (auch wenn dort Vibecoding-Projekte geplant werden) sind Momentaufnahmen für Menschen. Weder Claude Code noch ein Repo noch das Wiki stützt sich auf sie oder verweist auf sie als Beleg; was dort an Wissen entsteht, wird ins Wiki übernommen. Jede Datei in `docs/` trägt in ihrer ersten Zeile `<!-- heimat: repo — ändert sich mit: <Code-Pfad oder Guard> -->`; ein Konzept, das gerade gebaut wird, trägt stattdessen `<!-- heimat: repo — in Arbeit bis: JJJJ-MM-TT -->` und zieht bis dahin ins Wiki um. Aus Code und Doku wird auf Wiki-Seiten mit `Wiki „Seitentitel“ §n` verwiesen. Prüffrage vor jeder neuen Datei in `docs/`: *Muss sie sich ändern, wenn sich der Code ändert, oder prüft sie ein Guard?* Wenn nein, ist sie eine Wiki-Seite. Der Guard dieses Repos und der Todoteck-Job `wiki_repo_check` prüfen das.
<!-- /heimat-regel -->

In diesem Repo bleiben README, `CLAUDE.md` und die drei Dateien unter `docs/`:
`docs/protocol.md` und `docs/release.md` ändern sich mit Firmware und Workflows,
`docs/volcengine-asr.md` ist die Spezifikation der fremden Schnittstelle, die der Code
spricht. Die Marken prüft `scripts/check-docs.py`.

## Doku-Hygiene

Doku veraltet an drei Stellen, und alle drei sind Aussagen, die nichts
nachrechnet: die **Kopie** (eine abgeleitete Seite wiederholt einen Fakt,
dessen Heimat woanders liegt), die **Sollens-Regel** (ein Regelwerk
behauptet eine Praxis, die so nicht gelebt wird) und die **handgepflegte
Aufzählung** (eine Tabelle spiegelt eine Menge aus dem Code).

Verbindlich vor Doku-Änderungen und bei jedem Aufräum-Durchgang: Notiz
**„Behauptungen, die niemand prüft"** im Todoteck-Projekt `llm-wiki`
(per `search`/`get_note`) — Gegenmittel je Sorte und Prüfliste.

Kurzfassung für dieses Repo:
- Eine Regel hier beschreibt, was **tatsächlich passiert**. Weicht sie von
  der Praxis ab, wird die Regel korrigiert — nicht die Praxis behauptet.
- Was sich aus dem Code aufzählen lässt (Modul-, Route-, Tabellenlisten,
  Verzeichnisbäume), gehört in einen Test, nicht in Prosa. Den gibt es:
  `scripts/check-docs.py` vergleicht markierte Regionen
  (`<!-- doku-vertrag:name --> … <!-- /doku-vertrag -->`) gegen den Code und
  läuft in `.github/workflows/doku.yml`. Eine neue Aufzählung in der Doku
  bekommt dort einen Vertrag oder wird ein Zeiger — kein dritter Weg.
- Status („X von Y umgesetzt", „noch kein PR") gehört nach Todoteck oder in
  git — nicht in eine Datei, die beim Erledigen niemand anfasst.
