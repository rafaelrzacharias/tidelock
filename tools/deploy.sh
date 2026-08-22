#!/usr/bin/env bash
# Deploy a cross-built tree to a target and print the remote run line. Spec: docs/BUILD.md §7.
# Usage: tools/deploy.sh <preset> <host> [remote-dir]
set -euo pipefail

preset="${1:?usage: deploy.sh <preset> <host> [remote-dir]}"
host="${2:?usage: deploy.sh <preset> <host> [remote-dir]}"
remote="${3:-~/tidelock}"

bin="out/$preset/bin"
[ -d "$bin" ] || { echo "deploy: $bin does not exist - build the preset first" >&2; exit 1; }

ssh "$host" "mkdir -p '$remote'"
scp -r "$bin" "$host:$remote/"
for d in script assets; do
  [ -d "$d" ] && scp -r "$d" "$host:$remote/"
done
scp "out/$preset/build_id.txt" "$host:$remote/" 2>/dev/null || true

echo "deployed $preset -> $host:$remote"
echo "run: ssh $host 'cd $remote && ./bin/tl_tests --tag smoke'"
echo "     ssh $host 'cd $remote && ./bin/tl_driver --scene script/scenes/harness.luau --seed 1 --ticks 600 --csv out.csv'"
