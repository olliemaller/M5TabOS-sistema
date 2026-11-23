#!/bin/bash
# Releases a new version of M5tabOS. The rules for which number to bump are in
# docs/VERSIONAMENTO.md.
#
#   ferramentas/lancar.sh patch     0.7.0 -> 0.7.1  (fixes only)
#   ferramentas/lancar.sh minor     0.7.0 -> 0.8.0  (something new, or needs reflashing)
#   ferramentas/lancar.sh major     0.7.0 -> 1.0.0
#   ferramentas/lancar.sh 0.9.0     a chosen number (must be higher)
#
# What it does, in this order, and only after you confirm:
#   1. version.txt gets the new number;
#   2. in CHANGELOG.md, whatever is under "Nao lancado" (Unreleased) becomes the version's
#      section, with today's date, and an empty "Nao lancado" goes back to the top;
#   3. commit "Versao X.Y.Z" and annotated tag vX.Y.Z.
# It does not push anything: the push is up to you (the command is printed at the end).
#
# Refuses when: there are uncommitted changes, the number is not higher than the last
# released version, or the CHANGELOG's "Nao lancado" is empty - a version with
# nothing written about it is a version nobody knows what it brought.
set -euo pipefail
cd "$(dirname "$0")/.."

falha() { echo "lancar: $*" >&2; exit 1; }

[ $# -eq 1 ] || { sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

git diff --quiet && git diff --cached --quiet || falha "there are uncommitted changes - commit or stash them before releasing."

atual=$(tr -d ' \r\n' < version.txt)
[[ "$atual" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || falha "version.txt does not contain X.Y.Z: '$atual'"
ultima=$(git describe --tags --abbrev=0 --match 'v[0-9]*' 2>/dev/null | sed 's/^v//' || true)

# Base for the arithmetic: the higher of version.txt and the last tag.
base="$atual"
if [ -n "$ultima" ] && [ "$(printf '%s\n%s\n' "$ultima" "$atual" | sort -V | tail -1)" = "$ultima" ]; then
  base="$ultima"
fi
IFS=. read -r MA MI PA <<< "$base"

case "$1" in
  patch) nova="$MA.$MI.$((PA + 1))" ;;
  minor) nova="$MA.$((MI + 1)).0" ;;
  major) nova="$((MA + 1)).0.0" ;;
  *)     nova="$1" ;;
esac
[[ "$nova" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || falha "invalid version: '$nova' (use patch, minor, major or X.Y.Z)"

if [ -n "$ultima" ]; then
  [ "$nova" != "$ultima" ] && [ "$(printf '%s\n%s\n' "$ultima" "$nova" | sort -V | tail -1)" = "$nova" ] \
    || falha "$nova is not higher than the last released version ($ultima)."
fi
git rev-parse -q --verify "refs/tags/v$nova" >/dev/null && falha "tag v$nova already exists."

# "Nao lancado" (Unreleased) must have some content line before the next section.
# ([^]]* and not [aã]: macOS awk reads byte by byte, and "ã" is two bytes.)
conteudo=$(awk '/^## \[(N[^]]*o lan[^]]*ado|Unreleased)\]/{dentro=1; next} /^## \[/{dentro=0} dentro && NF' CHANGELOG.md | wc -l | tr -d ' ')
[ "$conteudo" -gt 0 ] || falha "the \"Nao lancado\" (Unreleased) section of CHANGELOG.md is empty - write what this version brings."

ramo=$(git rev-parse --abbrev-ref HEAD)
commits=$(git rev-list --count "${ultima:+v$ultima..}HEAD")
echo "M5tabOS: ${ultima:-none} -> $nova"
echo "  branch $ramo, $commits commits since the last version, $conteudo lines in the CHANGELOG"
[ "$ramo" = "main" ] || echo "  WARNING: version outside main."
read -r -p "Confirm? (s = yes) [s/N] " ok
[[ "$ok" =~ ^[sS]$ ]] || falha "cancelled."

hoje=$(date +%Y-%m-%d)
printf '%s\n' "$nova" > version.txt
awk -v v="$nova" -v d="$hoje" '
  !feito && /^## \[(N[^]]*o lan[^]]*ado|Unreleased)\]/ { print; print ""; print "## [" v "] - " d; feito=1; next }
  { print }' CHANGELOG.md > CHANGELOG.md.novo
mv CHANGELOG.md.novo CHANGELOG.md

git add version.txt CHANGELOG.md
git commit -q -m "Versao $nova"
git tag -a "v$nova" -m "M5tabOS $nova"

echo "Done: commit and tag v$nova created. To publish:"
echo "  git push origin $ramo v$nova"
