#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Installs the pre-commit hook. Run once after cloning.
set -euo pipefail
cd "$(dirname "$0")/../.."
hook=.git/hooks/pre-commit
cat > "$hook" <<'HOOK'
#!/usr/bin/env bash
set -e
exec ./tools/ci/check-no-game-data.sh --staged
HOOK
chmod +x "$hook"
echo "installed $hook"
