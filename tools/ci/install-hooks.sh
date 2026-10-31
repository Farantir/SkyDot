#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Enables the versioned hooks in tools/git-hooks. Run once after cloning.
set -euo pipefail
cd "$(dirname "$0")/../.."
git config core.hooksPath tools/git-hooks
echo "core.hooksPath = tools/git-hooks"
