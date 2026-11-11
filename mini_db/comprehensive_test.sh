#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
CXX="${CXX:-g++}"
"$CXX" -std=c++98 -Wall -Wextra -Werror mini_db.cpp -o mini_db
exec python3 comprehensive_test.py --binary ./mini_db "$@"
