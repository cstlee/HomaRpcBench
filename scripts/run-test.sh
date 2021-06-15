#! /bin/bash

if [[ $# -ne 3 ]]; then
    echo "Usage: run-test.sh <CONFIG> <TEST_NAME> <LOG_DIR>"
    exit 0
fi

CONFIG=$1
TEST=$2
LOG_DIR=$3

# CONFIG should define the following variables
#
# HOSTS - location of the file containing a list of remote machines to use
# BIN_DIR - full path to directory containing the test binaries
source $CONFIG

function remote_start {
    local REMOTE=$1
    local CMD=$2
    local OUT_PREFIX=$3
    ssh $REMOTE "nohup $CMD > $OUT_PREFIX.out.log 2> $OUT_PREFIX.err.log < /dev/null &"
}

function remote_stop {
    local REMOTE=$1
    local NAME=$2
    ssh $REMOTE "sudo killall -s SIGINT $NAME"
}

function extract_coordinator_mac {
    local REMOTE=$1
    local OUT_PREFIX=$2 
    ssh $REMOTE "grep 'DpdkDriver address: ' < $OUT_PREFIX.err.log | grep -o -E '([[:xdigit:]]{1,2}:){5}[[:xdigit:]]{1,2}'"
}

COORDINATOR_HOST="${HOSTS[0]}"
CLIENT_HOST="${HOSTS[1]}"
SERVER_HOSTS=${HOSTS[@]:2}

COORDINATOR="$BIN_DIR/coordinator"
CLIENT="$BIN_DIR/client"
SERVER="$BIN_DIR/server"

DATE_TIME=$(date +"%F-%H-%M-%S")

# Setup local log directories
mkdir -p $LOG_DIR
LOG_DIR=$(cd $LOG_DIR; pwd)
mkdir -p "$LOG_DIR/$DATE_TIME"
ln -sFfh "$LOG_DIR/$DATE_TIME" "$LOG_DIR/latest"

##### Start Coordinator
echo "Start Coordinator on $COORDINATOR_HOST"
ssh $COORDINATOR_HOST "mkdir -p ~/logs/$DATE_TIME"
remote_start $COORDINATOR_HOST "sudo $COORDINATOR 1 -vvv" "~/logs/$DATE_TIME/coordinator"
sleep 2
COORDINATOR_MAC=`extract_coordinator_mac $COORDINATOR_HOST "~/logs/$DATE_TIME/coordinator"`

##### Start Servers
SERVER_ID=1
for HOST in ${SERVER_HOSTS[@]}
do
    echo "Start Server $SERVER_ID on $HOST"
    ssh $HOST "mkdir -p ~/logs/$DATE_TIME"
    remote_start $HOST "sudo $SERVER 1 $COORDINATOR_MAC -vvv --timetrace ~/logs/$DATE_TIME" "~/logs/$DATE_TIME/server-$SERVER_ID"
    let "SERVER_ID++"
    sleep 0.5
done

##### Run Client
echo "Run Client on $CLIENT_HOST"
ssh $CLIENT_HOST "mkdir -p ~/logs/$DATE_TIME"
ssh $CLIENT_HOST "sudo $CLIENT 1 $COORDINATOR_MAC $TEST -vvv --timetrace ~/logs/$DATE_TIME" > "$LOG_DIR/$DATE_TIME/client.out.log" 2> "$LOG_DIR/$DATE_TIME/client.err.log"

##### Collect Logs
# Coordinator Logs
echo "Copying Coordinator logs from $COORDINATOR_HOST"
scp "$COORDINATOR_HOST:~/logs/$DATE_TIME/*" "$LOG_DIR/$DATE_TIME"
# Client Logs
echo "Copying Client logs from $CLIENT_HOST"
scp "$CLIENT_HOST:~/logs/$DATE_TIME/*" "$LOG_DIR/$DATE_TIME"
# Server Logs
SERVER_ID=1
for HOST in ${SERVER_HOSTS[@]}
do
    echo "Copying Server $SERVER_ID logs from $HOST"
    scp "$HOST:~/logs/$DATE_TIME/*" "$LOG_DIR/$DATE_TIME"
    let "SERVER_ID++"
done

##### Stop Servers
SERVER_ID=1
for HOST in ${SERVER_HOSTS[@]}
do
    echo "Stop Server $SERVER_ID on $HOST"
    remote_stop $HOST $SERVER
    let "SERVER_ID++"
    sleep 0.5
done

##### Stop Coordinator
echo "Stop Coordinator on $COORDINATOR_HOST"
remote_stop $COORDINATOR_HOST "$COORDINATOR"
