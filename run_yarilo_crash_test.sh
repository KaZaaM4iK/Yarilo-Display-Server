#!/usr/bin/env bash
set -u

echo "================================================="
echo "   Yarilo A2: Client Crash / SIGKILL Test       "
echo "================================================="

SERVER_PID=0
WM_PID=0
CLIENT1_PID=0
CLIENT2_PID=0

TEST_RUNTIME_DIR=""
SERVER_LOG=""

cleanup() {
    local exit_code=$?
    trap - EXIT
    trap ':' INT TERM
    echo "==> [CLEANUP] Stopping running processes..."

    wait_group() {
        local pids="$1"
        local label="$2"
        # SIGTERM
        local sent=0
        for pid in $pids; do
            if [ "$pid" -gt 0 ] && kill -0 "$pid" 2>/dev/null; then
                kill -15 "$pid" 2>/dev/null || true
                sent=1
            fi
        done
        if [ "$sent" -eq 1 ]; then
            for ((attempt = 0; attempt < 10; attempt++)); do
                local running=0
                for pid in $pids; do
                    if [ "$pid" -gt 0 ] && kill -0 "$pid" 2>/dev/null; then running=1; break; fi
                done
                if [ "$running" -eq 0 ]; then return 0; fi
                sleep 0.1
            done
        fi
        # SIGKILL
        for pid in $pids; do
            if [ "$pid" -gt 0 ] && kill -0 "$pid" 2>/dev/null; then
                echo "==> [CLEANUP] Force killing $label PID $pid"
                kill -9 "$pid" 2>/dev/null || true
            fi
        done
        # Wait after SIGKILL
        for ((attempt = 0; attempt < 10; attempt++)); do
            local running=0
            for pid in $pids; do
                if [ "$pid" -gt 0 ] && kill -0 "$pid" 2>/dev/null; then running=1; break; fi
            done
            if [ "$running" -eq 0 ]; then return 0; fi
            sleep 0.1
        done
        return 1
    }

    wait_group "$CLIENT1_PID $CLIENT2_PID" "client"
    wait_group "$WM_PID" "WM"
    wait_group "$SERVER_PID" "server"

    # Финальная проверка: не остались ли процессы
    local remaining=0
    for pid in "$CLIENT1_PID" "$CLIENT2_PID" "$WM_PID" "$SERVER_PID"; do
        if [ "$pid" -gt 0 ] && kill -0 "$pid" 2>/dev/null; then
            echo "==> [CLEANUP] Process PID $pid still alive after SIGKILL"
            remaining=1
        fi
    done
    if [ "$remaining" -eq 1 ]; then
        echo "==> [CLEANUP] Some processes did not terminate. Preserving diagnostic artifacts."
        exit_code=1
    fi

    if [ "$exit_code" -ne 0 ]; then
        echo "==> [CLEANUP] Test FAILED (exit code $exit_code). Preserving diagnostic artifacts:"
        echo "    Test runtime directory: $TEST_RUNTIME_DIR"
        echo "    Server log:             $SERVER_LOG"
        echo "==> [CLEANUP] Last server log lines:"
        tail -n 20 "$SERVER_LOG" 2>/dev/null || true
    else
        if [ -n "$TEST_RUNTIME_DIR" ]; then rm -rf "$TEST_RUNTIME_DIR"; fi
    fi
    exit "$exit_code"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

TEST_RUNTIME_DIR=$(mktemp -d -t yarilo-test-a2-XXXXXX) || {
    echo "[FAIL] Cannot create test runtime directory"
    exit 1
}
chmod 0700 "$TEST_RUNTIME_DIR" || {
    rm -rf "$TEST_RUNTIME_DIR"
    TEST_RUNTIME_DIR=""
    echo "[FAIL] Cannot set permissions on test runtime directory"
    exit 1
}
export XDG_RUNTIME_DIR="$TEST_RUNTIME_DIR"
SERVER_LOG="$TEST_RUNTIME_DIR/server.log"

cmake --build build -j2 || exit 1

SOCKET_PATH="$TEST_RUNTIME_DIR/yarilo-0"
./build/yarilo > >(tee -a "$SERVER_LOG") 2>&1 &
SERVER_PID=$!

for i in {1..50}; do
    if [ -S "$SOCKET_PATH" ]; then break; fi
    sleep 0.1
done

if [ ! -S "$SOCKET_PATH" ]; then
    echo "[FAIL] Server IPC socket did not appear!"
    exit 1
fi
sleep 0.2

echo "Starting Window Manager..."
./build/yarilo_wm &
WM_PID=$!

WM_READY=0
for i in {1..30}; do
    if grep -q "promoted to Window Manager (0 pending maps queued)" "$SERVER_LOG" 2>/dev/null; then
        WM_READY=1
        break
    fi
    if ! kill -0 "$WM_PID" 2>/dev/null; then break; fi
    sleep 0.1
done

if [ "$WM_READY" -ne 1 ]; then
    echo "[FAIL] Window Manager failed to establish authenticated session!"
    exit 1
fi
echo "[OK] Window Manager promoted and ready."

echo "Starting Client 1 (Victim: --crash-after-commits 50)..."
./build/yarilo_client 1 --crash-after-commits 50 &
CLIENT1_PID=$!

echo "Starting Client 2 (Survivor: 300 frames)..."
./build/yarilo_client 2 &
CLIENT2_PID=$!

CLIENTS_TIMEOUT=100
while [ "$CLIENTS_TIMEOUT" -gt 0 ]; do
    c1=0; c2=0
    if kill -0 "$CLIENT1_PID" 2>/dev/null; then c1=1; fi
    if kill -0 "$CLIENT2_PID" 2>/dev/null; then c2=1; fi
    if [ "$c1" -eq 0 ] && [ "$c2" -eq 0 ]; then break; fi
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

if [ "$CLIENT1_STATUS" -ne 137 ]; then
    echo "[FAIL] Client 1 exited with status $CLIENT1_STATUS, expected 137 (SIGKILL)!"
    exit 1
fi
echo "[OK] Client 1 successfully terminated via SIGKILL (exit code 137)."

wait "$CLIENT2_PID"
CLIENT2_STATUS=$?
CLIENT2_PID=0

if [ "$CLIENT2_STATUS" -ne 0 ]; then
    echo "[FAIL] Client 2 failed with exit code $CLIENT2_STATUS!"
    exit 1
fi
echo "[OK] Client 2 completed all 300 frames with PASS."

SERVER_TIMEOUT=180
while [ "$SERVER_TIMEOUT" -gt 0 ]; do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then break; fi
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

echo "================================================="
if [ "$SERVER_STATUS" -eq 0 ]; then
    echo "  [PASS] A2: Client Crash / SIGKILL Test SUCCEEDED!"
    exit 0
else
    echo "  [FAIL] Server exited with error code $SERVER_STATUS"
    exit 1
fi