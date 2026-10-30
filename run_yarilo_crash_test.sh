#!/usr/bin/env bash
set -u

echo "================================================="
echo "   Yarilo A2: Client Crash / SIGKILL Test       "
echo "================================================="

# 1. Сборка
cmake --build build -j2 || exit 1

# 2. Запуск сервера в фоне
./build/yarilo &
SERVER_PID=$!

# Ожидание сокета IPC
SOCKET_PATH="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/yarilo-0"
for i in {1..30}; do
    if [ -S "$SOCKET_PATH" ]; then
        break
    fi
    sleep 0.1
done

if [ ! -S "$SOCKET_PATH" ]; then
    echo "[FAIL] Server IPC socket did not appear!"
    kill -9 "$SERVER_PID" 2>/dev/null
    exit 1
fi

# 3. Запуск Window Manager
./build/yarilo_wm &
WM_PID=$!
sleep 0.3

# 4. Запуск Клиента 1 (Жертва: падение через SIGKILL после 50 подтвержденных коммитов)
echo "Starting Client 1 (Victim: --crash-after-commits 50)..."
./build/yarilo_client 1 --crash-after-commits 50 &
CLIENT1_PID=$!

# 5. Запуск Клиента 2 (Выживший: стриминг 300 кадров)
echo "Starting Client 2 (Survivor: 300 frames)..."
./build/yarilo_client 2 &
CLIENT2_PID=$!

# 6. Ожидание Клиента 1 (ожидаем строго код 137 от SIGKILL)
set +e
wait "$CLIENT1_PID"
CLIENT1_STATUS=$?
set -e

if [ "$CLIENT1_STATUS" -ne 137 ]; then
    echo "[FAIL] Client 1 exited with status $CLIENT1_STATUS, expected 137 (SIGKILL)!"
    kill -9 "$CLIENT2_PID" "$WM_PID" "$SERVER_PID" 2>/dev/null
    exit 1
fi
echo "[OK] Client 1 successfully terminated via SIGKILL (exit code 137)."

# 7. Ожидание Клиента 2 (ожидаем строго 0 / PASS)
wait "$CLIENT2_PID"
CLIENT2_STATUS=$?

if [ "$CLIENT2_STATUS" -ne 0 ]; then
    echo "[FAIL] Client 2 failed with exit code $CLIENT2_STATUS!"
    kill -9 "$WM_PID" "$SERVER_PID" 2>/dev/null
    exit 1
fi
echo "[OK] Client 2 completed all 300 frames with PASS."

# 8. Ожидание штатного завершения сервера (таймаут 15с)
wait "$SERVER_PID"
SERVER_STATUS=$?

wait "$WM_PID" 2>/dev/null || true

echo "================================================="
if [ "$SERVER_STATUS" -eq 0 ]; then
    echo "  [PASS] A2: Client Crash / SIGKILL Test SUCCEEDED!"
    echo "================================================="
    exit 0
else
    echo "  [FAIL] Server exited with error code $SERVER_STATUS"
    echo "================================================="
    exit 1
fi