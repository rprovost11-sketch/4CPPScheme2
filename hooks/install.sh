#!/bin/sh
# Activate the tracked pre-push hook for this clone (run once after cloning).
cd "$(git rev-parse --show-toplevel)" && git config core.hooksPath hooks
echo "pre-push hook enabled (core.hooksPath=hooks). Bypass a push with --no-verify."
