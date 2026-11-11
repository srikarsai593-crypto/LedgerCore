#!/usr/bin/env bash
set -euo pipefail
make -s crash-test
exec ./bin/ledgercore_crash_test "${CRASH_ITERS:-20}"
