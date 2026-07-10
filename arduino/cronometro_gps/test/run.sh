#!/usr/bin/env bash
# Compila ed esegue gli unit test della logica pura del cronometro.
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Wextra -O2 -o test_logic test_logic.cpp
./test_logic
