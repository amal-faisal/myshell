#!/bin/bash

PORT=$(cat .myshell_port 2>/dev/null || echo "8080")

run_scenario() {
    local scenario_num=$1
    local duration=$2
    shift 2
    
    echo ""
    echo "======== TEST SCENARIO $scenario_num ========"
    rm -f server.log
    ./server > server.log 2>&1 &
    SERVER_PID=$!
    sleep 1
    
    # Run the clients passed as arguments
    local i=0
    for cmd_group in "$@"; do
        i=$((i+1))
        eval "$cmd_group" &
    done
    
    sleep $duration
    killall client 2>/dev/null || true
    kill $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
    sleep 0.5
    
    echo "Server Log:"
    cat server.log | grep "^\["
}

# Scenario 1: 3 concurrent clients with single commands
run_scenario 1 3 \
    '(echo "pwd"; sleep 10) | ./client '$PORT 2>&1 > /tmp/s1_c1.out' \
    '(echo "echo Test"; sleep 10) | ./client '$PORT 2>&1 > /tmp/s1_c2.out' \
    '(echo "pwd"; sleep 10) | ./client '$PORT 2>&1 > /tmp/s1_c3.out'

# Scenario 2: Mixed - one single command, two demo programs
run_scenario 2 8 \
    '(echo "pwd"; sleep 10) | ./client '$PORT 2>&1 > /tmp/s2_c1.out' \
    '(echo "demo 3"; sleep 10) | ./client '$PORT 2>&1 > /tmp/s2_c2.out' \
    '(echo "demo 2"; sleep 10) | ./client '$PORT 2>&1 > /tmp/s2_c3.out'

# Scenario 3: All demo programs with different durations  
run_scenario 3 15 \
    '(echo "demo 3"; sleep 20) | ./client '$PORT 2>&1 > /tmp/s3_c1.out' \
    '(echo "demo 2"; sleep 20) | ./client '$PORT 2>&1 > /tmp/s3_c2.out' \
    '(echo "demo 4"; sleep 20) | ./client '$PORT 2>&1 > /tmp/s3_c3.out'

echo ""
echo "======== Test Complete ========"
