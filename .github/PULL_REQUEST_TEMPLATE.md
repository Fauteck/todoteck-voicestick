<!-- Fauteck-PR-Vorlage v1 — der Kern ist in allen Fauteck-Repos wortgleich; nur der Abschnitt „Repo-spezifisch“ unterscheidet sich. Nicht Zutreffendes löschen statt leer stehen lassen. -->

## Zweck

<!-- Warum gibt es diesen PR? Ein bis drei Sätze. Bezug: Issue, Wiki „Seitentitel“ §n -->

## Umfang

<!-- Was ist geändert — und was bewusst nicht? -->

**Art:** <!-- Feature · Bugfix · Refactoring/Chore · Doku · CI/Build · Abhängigkeiten -->

## Geprüft

<!-- Welche Befehle liefen, mit welchem Ergebnis? Wo kein CI am PR läuft, ist das der einzige Beleg vor dem Merge. -->

| Befehl | Ergebnis |
|---|---|
| `…` | ✅ / ❌ |

## Breaking Changes und Risiken

- [ ] Keine
- [ ] Ja: <!-- API-Vertrag, Schema/Migration, ENV-Variable, Datenformat, Verhalten entfernt … -->

## Nach dem Merge

- [ ] Nichts zu tun
- [ ] Image bauen (Publish-Workflow von Hand anstoßen) und Redeploy in docker-configs
- [ ] Wiki-Seite pflegen: <!-- Titel -->
- [ ] Sonstiges: <!-- … -->

## Checkliste

- [ ] Keine Secrets im Diff, keine Tokens in Logs, `.env` nicht committet
- [ ] Neue oder geänderte ENV-Variable in `.env.example` (und README)
- [ ] Schemaänderung nur über eine neue Migration
- [ ] README bzw. code-gebundene Doku im selben PR aktualisiert, falls betroffen
- [ ] Konzepte und Entscheidungen ins Wiki statt nach `docs/` (Heimat-Regel)
- [ ] Deutsche Texte mit Umlauten (ä, ö, ü, ß)
- [ ] Keine Debug-Ausgaben, keine temporären Workarounds

## Repo-spezifisch

- [ ] `python3 scripts/check-docs.py` grün
- [ ] Protokoll geändert → `docs/protocol.md` im selben PR
- [ ] Firmware-Version geändert → `VERSION` und `firmware/version.txt` stimmen überein
