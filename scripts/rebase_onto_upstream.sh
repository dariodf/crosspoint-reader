#!/usr/bin/env bash
# Rebases the current branch onto an upstream release tag and sets the fork
# version to <tag>-findmode.<N>, carrying N from the current version.
#
# Fork commits that bump the version line in platformio.ini conflict with every
# upstream version bump. The script takes upstream's line for those, and any
# other conflict aborts the rebase with a non-zero exit.
#
# Usage: scripts/rebase_onto_upstream.sh <upstream-tag>
set -euo pipefail

tag="$1"
findmode="$(sed -n 's/^version = .*-findmode\.//p' platformio.ini | head -n1)"
if [ -z "$findmode" ]; then
  echo "platformio.ini version has no -findmode.<N> suffix" >&2
  exit 1
fi
version="${tag#v}-findmode.${findmode}"

resolve_version_line() {
  [ "$(git diff --name-only --diff-filter=U)" = "platformio.ini" ] || return 1
  # Keeps the upstream side of each conflict when both sides are version lines.
  awk '
    /^<<<<<<< / { side = 1; next }
    /^=======$/ && side == 1 { side = 2; next }
    /^>>>>>>> / && side == 2 { side = 0; next }
    side == 1 { if ($0 !~ /^version = /) bad = 1; print; next }
    side == 2 { if ($0 !~ /^version = /) bad = 1; next }
    { print }
    END { exit bad }
  ' platformio.ini > platformio.ini.resolved || { rm -f platformio.ini.resolved; return 1; }
  mv platformio.ini.resolved platformio.ini
  git add platformio.ini
}

if ! git rebase "$tag"; then
  while [ -d "$(git rev-parse --git-path rebase-merge)" ]; do
    if ! resolve_version_line; then
      echo "Conflicts beyond the version line:" >&2
      git diff --name-only --diff-filter=U >&2
      git rebase --abort
      exit 1
    fi
    # A version-only commit is empty once upstream's line wins.
    if git diff --cached --quiet; then
      git rebase --skip || true
    else
      GIT_EDITOR=true git rebase --continue || true
    fi
  done
fi

if ! git merge-base --is-ancestor "$tag" HEAD; then
  echo "Rebase onto ${tag} did not complete" >&2
  exit 1
fi

sed -i.bak "s/^version = .*/version = ${version}/" platformio.ini
rm platformio.ini.bak
git commit -m "chore: ${version}" platformio.ini
echo "version=${version}"
