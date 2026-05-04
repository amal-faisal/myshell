#!/bin/bash

PORT=$(cat .myshell_port 2>/dev/null || echo "8080")
SCENARIO=$1

test_scenario_1() {
    echo "======== SCENARIO 1: 3 clients, single commands ========"
    rm -f server.log
    ./server > server.log 2>&1 &
    SERVER_PID=$!
    sleep 1
    
    (echo "pwd"; sleep 10) | ./client $PORT 2>&1 > /tmp/s1_c1.out &
    sleep 0.2
    (echo "echo TEST"; sleep 10) | ./client $PORT 2>&1 > /tmp/s1_c2.out &
    sleep 0.2
    (echo "pwd"; sleep 10) | ./client $PORT 2>&1 > /tmp/s1_c3.out &
    
    sleep 3
    killall client 2>/dev/null || true
    kill $SERVER_PID 2>/dev/null || true
    wait 2>/dev/null || true
    
    echo "Server Log:"
    cat server.log | grep "^\[" | head -30
}

test_scenario_2() {
    echo "======== SCENARIO 2: 1 single cmd + 2 demo tasks ========"
    rm -f server.log
    ./server > server.log 2>&1 &
    SERVER_PID=$!
    sleep 1
    
    (echo "pwd"; sleep 10) | ./client $PORT 2>&1 > /tmp/s2_c1.out &
    sleep 0.2
    (echo "demo 3"; sleep 10) | ./client $PORT 2>&1 > /tmp/s2_c2.out &
    sleep 0.2
    (echo "demo 2"; sleep 10) | ./client $PORT 2>&1 > /tmp/s2_c3.out &
    
    sleep 8
    killall client 2>/dev/null || true
    kill $SERVER_PID 2>/dev/null || true
    wait 2>/dev/null || true
    
    echo "Server Log:"
    cat server.log | grep "^\[" | head -40
}

test_scenario_3() {
    echo "======== SCENARIO 3: 3 demo tasks different sizes ========"
    rm -f server.log
    ./server > server.log 2>&1 &
    SERVER_PID=$!
    sleep 1
    
    (echo "demo 3"; sleep 20) | ./client $PORT 2>&1 > /tmp/s3_c1.out &
    sleep 0.2
    (echo "demo 2"; sleep 20) | ./client $PORT 2>&1 > /tmp/s3_c2.out &
    sleep 0.2
    (echo "demo 4"; sleep 20) | ./client $PORT 2>&1 > /tmp/s3_c3.out &
    
    sleep 12
    killall client 2>/dev/null || true
    kill $SERVER_PID 2>/dev/null || true
    wait 2>/dev/null || true
    
    echo "Server Log:"
    cat server.log | grep "^\["
}

case "$SCENARIO" in
    1) test_scenario_1 ;;
    2) test_scenario_2 ;;
    3) test_scenario_3 ;;
    *) echo "Usage: $0 <1|2|3>"; exit 1 ;;
esac
