#!/bin/bash
# Logs every Claude Code hook payload (received on stdin) to /tmp/cc-hooks.log, one JSON per line.
# Use this FIRST (via hooks.capture.settings.json) to see the real event names + fields your
# Claude Code version emits, before trusting feldd-cc's hook->LED map.
{ cat; echo; } >> /tmp/cc-hooks.log 2>/dev/null
exit 0
