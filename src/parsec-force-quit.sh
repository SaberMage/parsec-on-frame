#!/bin/bash
# Force-quits Parsec. A hung Parsec ignores the polite quit (SIGTERM) that the
# dashboard sends, so this uses SIGKILL.
HERE=$(dirname "$(readlink -f "$0")")
exec >>"$HOME/.parsec/force-quit.log" 2>&1
echo "$(date '+%F %T') force quit requested; PATH=$PATH"
pgrep -af "^$HERE/parsecd"
pkill -KILL -f "^$HERE/parsecd" && echo "killed" || echo "nothing to kill (rc=$?)"
