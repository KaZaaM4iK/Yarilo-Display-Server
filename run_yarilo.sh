#!/bin/bash
set -e

echo "Building project..."
cmake --build build

echo "Starting Yarilo Server..."
./build/yarilo &
SERVER_PID=$!

sleep 1

echo "Starting Yarilo Window Manager..."
./build/yarilo_wm &
WM_PID=$!

sleep 1

echo "Starting Client 1 (Left Slot)..."
./build/yarilo_client &
CLIENT1_PID=$!

sleep 0.5

echo "Starting Client 2 (Right Slot)..."
./build/yarilo_client &
CLIENT2_PID=$!

# Ждем завершения обоих клиентов
wait $CLIENT1_PID
wait $CLIENT2_PID

# Ждем завершения сервера
wait $SERVER_PID

kill $WM_PID 2>/dev/null || true

echo "Two-client test finished successfully."