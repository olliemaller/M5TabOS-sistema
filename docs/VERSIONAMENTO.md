# Versioning

M5tabOS follows [SemVer](https://semver.org/): `MAJOR.MINOR.PATCH`. The number
lives in `version.txt` and only changes through `ferramentas/lancar.sh`.

## Which number to bump

While the version starts with **0.**, the system is still taking shape. The
rule is:

| What the version brings | Bump | Example |
|---|---|---|
| Only fixes, nothing new for the user | PATCH (`patch`) | 0.7.0 → 0.7.1 |
| Anything new: app, screen, setting, service | MINOR (`minor`) | 0.7.1 → 0.8.0 |
| Something that **requires reflashing with `./gravar.sh`** (partition table, NVS format, a card file format the old version cannot read) | MINOR (`minor`), and the CHANGELOG says so in bold | 0.8.0 → 0.9.0 |
| The system is considered stable and the app API (`kernel/app.h`) becomes a commitment | MAJOR: **1.0.0** | 0.x → 1.0.0 |

After 1.0.0, a change that breaks existing apps, or that requires reflashing,
bumps MAJOR.

When in doubt between two numbers, use the bigger one. One number too many
costs nothing. One too few hides a change.

## Day to day

1. Every change someone will notice gets a line in `CHANGELOG.md`, in the
   **Unreleased** section, in the same commit as the change.
2. When what is there is worth a version, run:
   ```bash
   ferramentas/lancar.sh minor
   ```
   The script shows which version it goes from and to and asks for
   confirmation. Then it writes `version.txt`, turns "Unreleased" into the
   version's section with the date, and creates the commit `Versao X.Y.Z` and
   the tag `vX.Y.Z`.
3. Publish: `git push origin main vX.Y.Z`.

## What makes sure the version only moves forward

- `lancar.sh` refuses a number that is not greater than the last tag, refuses
  to release with uncommitted changes and refuses to release with an empty
  "Unreleased".
- The build (`CMakeLists.txt`) **stops with an error** if `version.txt` is
  below the last released version.
- The number built into the binary says where the build came from:
  - `0.8.0`: exactly the released version;
  - `0.8.0+12.a1b2c3d`: 12 commits after it, at commit `a1b2c3d`.

  That number appears in Settings, on the console (`status`) and in the boot
  log, so a device with a development build never passes for a released
  version. Uncommitted changes do not show in the number: `+N.hash` means
  "built from".
