#!/usr/bin/env bash
set -u

echo "================================================="
echo "   Yarilo Baseline Integration Smoke Test        "
echo "================================================="

SERVER_PID=0
WM_PID=0
CLIENT1_PID=0
CLIENT2_PID=0

cleanup() {
    local exit_code=$?
    trap - EXIT INT TERM
    for pid in "$CLIENT1_PID" "$CLIENT2_PID" "$WM_PID" "$SERVER_PID"; do
        if [ "$pid" -gt 0 ] && kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
    wait 2>/dev/null || true
    exit "$exit_code"
}
trap cleanup EXIT INT TERM

# 1. Сборка
cmake --build build -j2 || exit 1

# 2. Запуск сервера
SOCKET_PATH="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/yarilo-0"
rm -f "$SOCKET_PATH"

./build/yarilo &
SERVER_PID=$!

for i in {1..50}; do
    if [ -S "$SOCKET_PATH" ]; then break; fi
    sleep 0.1
done

if [ ! -S "$SOCKET_PATH" ]; then
    echo "[FAIL] Server IPC socket did not appear!"
    exit 1
fi
sleep 0.3

# 3. Запуск Window Manager
./build/yarilo_wm &
WM_PID=$!
sleep 0.3

if ! kill -0 "$WM_PID" 2>/dev/null; then
    echo "[FAIL] Window Manager crashed on startup!"
    exit 1
fi

# 4. Запуск двух клиентов (полные 300 кадров)
./build/yarilo_client 1 &
CLIENT1_PID=$!

./build/yarilo_client 2 &
CLIENT2_PID=$!

# 5. Ожидание завершения клиентов
wait "$CLIENT1_PID"
C1_STATUS=$?
CLIENT1_PID=0

wait "$CLIENT2_PID"
C2_STATUS=$?
CLIENT2_PID=0

if [ "$C1_STATUS" -ne 0 ] || [ "$C2_STATUS" -ne 0 ]; then
    echo "[FAIL] Clients failed! Client 1: $C1_STATUS, Client 2: $C2_STATUS"
    exit 1
fi

# 6. Ожидание штатного завершения сервера (15с лимит)
wait "$SERVER_PID"
SERVER_STATUS=$?
SERVER_PID=0

if [ "$WM_PID" -gt 0 ]; then
    kill -15 "$WM_PID" 2>/dev/null || true
    wait "$WM_PID" 2>/dev/null || true
    WM_PID=0
fi

echo "================================================="
if [ "$SERVER_STATUS" -eq 0 ]; then
    echo "  [PASS] Yarilo Baseline Smoke Test SUCCEEDED!   "
    echo "================================================="
    exit 0
else
    echo "  [FAIL] Server exited with error code $SERVER_STATUS"
    echo "================================================="
    exit 1
fi