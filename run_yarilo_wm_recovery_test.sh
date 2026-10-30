#!/usr/bin/env bash
set -u

echo "================================================="
echo "   Yarilo B2: Window Manager Crash & Recovery   "
echo "================================================="

SERVER_PID=0
WM1_PID=0
WM2_PID=0
CLIENT1_PID=0
CLIENT2_PID=0

cleanup() {
    local exit_code=$?
    trap - EXIT INT TERM
    echo "==> [CLEANUP] Stopping all running processes..."
    for pid in "$CLIENT1_PID" "$CLIENT2_PID" "$WM1_PID" "$WM2_PID" "$SERVER_PID"; do
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

# 2. Удаление старого сокета и запуск сервера в фоне
SOCKET_PATH="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/yarilo-0"
rm -f "$SOCKET_PATH"

./build/yarilo &
SERVER_PID=$!

# Ожидание создания НОВОГО сокета сервером
for i in {1..50}; do
    if [ -S "$SOCKET_PATH" ]; then break; fi
    sleep 0.1
done

if [ ! -S "$SOCKET_PATH" ]; then
    echo "[FAIL] Server IPC socket did not appear!"
    exit 1
fi
sleep 0.3

# 3. Запуск WM #1
echo "Starting Window Manager #1..."
./build/yarilo_wm &
WM1_PID=$!
sleep 0.3

if ! kill -0 "$WM1_PID" 2>/dev/null; then
    echo "[FAIL] Window Manager #1 crashed immediately on startup!"
    exit 1
fi

# 4. Запуск Клиента 1 и Клиента 2 (с флагом --allow-dropped-frames)
echo "Starting Client 1 (300 frames, resilient to unmapped drops)..."
./build/yarilo_client 1 --allow-dropped-frames &
CLIENT1_PID=$!

echo "Starting Client 2 (300 frames, resilient to unmapped drops)..."
./build/yarilo_client 2 --allow-dropped-frames &
CLIENT2_PID=$!

# Даем клиентам отрендерить начальную пачку кадров (~1.2 сек)
sleep 1.2

# 5. УБИВАЕМ WM #1 через SIGKILL
echo "==> [CRASH] Killing Window Manager #1 via SIGKILL..."
kill -9 "$WM1_PID" 2>/dev/null
wait "$WM1_PID" 2>/dev/null || true
WM1_PID=0

# Пауза для обработки разрыва сервером (сброс active_wm_session_id)
sleep 0.4

# 6. ЗАПУСКАЕМ WM #2 (Recovery)
echo "==> [RECOVERY] Starting Window Manager #2 (Reconnection)..."
./build/yarilo_wm &
WM2_PID=$!
sleep 0.3

if ! kill -0 "$WM2_PID" 2>/dev/null; then
    echo "[FAIL] Window Manager #2 failed to reconnect!"
    exit 1
fi

# 7. Ожидание завершения клиентов с таймаутом (Watchdog: 10 секунд)
echo "Waiting for clients to finish..."
CLIENTS_TIMEOUT=100
while [ "$CLIENTS_TIMEOUT" -gt 0 ]; do
    c1_running=0
    c2_running=0
    if kill -0 "$CLIENT1_PID" 2>/dev/null; then c1_running=1; fi
    if kill -0 "$CLIENT2_PID" 2>/dev/null; then c2_running=1; fi
    if [ "$c1_running" -eq 0 ] && [ "$c2_running" -eq 0 ]; then
        break
    fi
    sleep 0.1
    CLIENTS_TIMEOUT=$((CLIENTS_TIMEOUT - 1))
done

if [ "$CLIENTS_TIMEOUT" -le 0 ]; then
    echo "[FAIL] Timed out waiting for clients to complete streaming!"
    exit 1
fi

wait "$CLIENT1_PID"
CLIENT1_STATUS=$?
CLIENT1_PID=0

wait "$CLIENT2_PID"
CLIENT2_STATUS=$?
CLIENT2_PID=0

if [ "$CLIENT1_STATUS" -ne 0 ] || [ "$CLIENT2_STATUS" -ne 0 ]; then
    echo "[FAIL] Clients failed! Client 1: $CLIENT1_STATUS, Client 2: $CLIENT2_STATUS"
    exit 1
fi

echo "[OK] Both clients finished with PASS."

# 8. Ожидание штатного завершения сервера (таймаут 12 секунд, с запасом перекрывающий 15с рантайм сервера)
SERVER_TIMEOUT=120
while [ "$SERVER_TIMEOUT" -gt 0 ]; do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        break
    fi
    sleep 0.1
    SERVER_TIMEOUT=$((SERVER_TIMEOUT - 1))
done

if [ "$SERVER_TIMEOUT" -le 0 ]; then
    echo "[FAIL] Server hung and did not complete within timeout!"
    exit 1
fi

wait "$SERVER_PID"
SERVER_STATUS=$?
SERVER_PID=0

if [ "$WM2_PID" -gt 0 ]; then
    kill -15 "$WM2_PID" 2>/dev/null || true
    wait "$WM2_PID" 2>/dev/null || true
    WM2_PID=0
fi

echo "================================================="
if [ "$SERVER_STATUS" -eq 0 ]; then
    echo "  [PASS] B2: WM Crash & Recovery Test SUCCEEDED! "
    echo "================================================="
    exit 0
else
    echo "  [FAIL] Server exited with error code $SERVER_STATUS"
    echo "================================================="
    exit 1
fi