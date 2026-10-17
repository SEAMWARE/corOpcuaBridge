# SPDX-License-Identifier: Apache-2.0
#
# corTestFunctions.sh - the verbs this repo's tests use; corTest sources it into every section
#
# A test starts a broker that loads THIS build of the plugin (by path - nothing is installed), on a port
# of its own, with ramDB as its store: no database server is needed. A test waits on and asserts on what
# the API says, never on the broker's log - the log is kept for reading when a test fails.
#
#   OPCUA_BROKER       the broker binary (default: coraine on PATH - the coraine package, or
#                      `. <dir>/install/env` after coraine-build); AssetChannels need coraine 0.6 with
#                      COR_FEATURE_ASSET_CHANNELS
#   OPCUA_PORT         its port (default 19026 - clear of a broker on 1026 and of coraine's own test ports)
#   OPCUA_PLUGIN       the plugin under test (default: opcua.so in this repo)
#   OPCUA_SERVER       the tests' OPC UA server (default: test/opcuaTestServer in this repo - `make server`)
#   OPCUA_SERVER_PORT  its port (default 14840 - clear of a real server on 4840)
#   OPCUA_TEST_DIR     scratch directory: the bridge configuration, the server's fifo and writes, the pids and logs
#
OPCUA_REPO="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
OPCUA_BROKER="${OPCUA_BROKER:-coraine}"
OPCUA_PORT="${OPCUA_PORT:-19026}"
OPCUA_PLUGIN="${OPCUA_PLUGIN:-$OPCUA_REPO/opcua.so}"
OPCUA_SERVER="${OPCUA_SERVER:-$OPCUA_REPO/test/opcuaTestServer}"
OPCUA_SERVER_PORT="${OPCUA_SERVER_PORT:-14840}"
OPCUA_TEST_DIR="${OPCUA_TEST_DIR:-/tmp/opcua-bridge-test}"


# -----------------------------------------------------------------------------
#
# brokerStart [broker options...] - start the broker with the plugin and $OPCUA_TEST_DIR/bridges.json
#
# Returns once the port answers; 1 (with the log's tail on stderr) if the broker died on the way.
#
brokerStart() {
  brokerStop

  "$OPCUA_BROKER" --port "$OPCUA_PORT" --foreground --pretty-print 2 --httpEndpoint "http://localhost:$OPCUA_PORT" \
                 --database ramDB --bridges "$OPCUA_PLUGIN" --bridgeConfig "$OPCUA_TEST_DIR/bridges.json" "$@" \
                 > "$OPCUA_TEST_DIR/broker.log" 2>&1 &
  echo $! > "$OPCUA_TEST_DIR/broker.pid"

  local i
  for ((i = 0; i < 100; i++)); do
    curl -s -o /dev/null "localhost:$OPCUA_PORT/ngsi-ld/v1/types" && return 0
    if ! kill -0 "$(cat "$OPCUA_TEST_DIR/broker.pid")" 2>/dev/null; then
      echo "brokerStart: the broker died - $OPCUA_TEST_DIR/broker.log:" >&2
      tail -20 "$OPCUA_TEST_DIR/broker.log" >&2
      return 1
    fi
    sleep 0.1
  done

  echo "brokerStart: no answer on port $OPCUA_PORT after 10s" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# brokerStop - stop the broker brokerStart started (by its pid, never by name)
#
brokerStop() {
  local pidFile="$OPCUA_TEST_DIR/broker.pid"
  [ -f "$pidFile" ] || return 0

  local pid i
  pid=$(cat "$pidFile")
  kill "$pid" 2>/dev/null
  for ((i = 0; i < 50; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  kill -9 "$pid" 2>/dev/null
  rm -f "$pidFile"
}


# -----------------------------------------------------------------------------
#
# serverStart [cert dir] - the tests' OPC UA server (test/opcuaTestServer - `make server`), on $OPCUA_SERVER_PORT
#
# With a [cert dir] (made by `$OPCUA_SERVER --makeCerts <dir>`), the server is secure - see
# test/opcuaTestServer.c.
#
# Its address space is described in test/opcuaTestServer.c: a press (an AssetChannel's model) and a tank
# (plain Channels). Returns once the port answers.
#
serverStart() {
  serverStop

  rm -f "$OPCUA_TEST_DIR/server.cmd"
  mkfifo "$OPCUA_TEST_DIR/server.cmd"
  : > "$OPCUA_TEST_DIR/server.writes"

  "$OPCUA_SERVER" "$OPCUA_SERVER_PORT" "$OPCUA_TEST_DIR/server.cmd" "$OPCUA_TEST_DIR/server.writes" $1 \
                  > "$OPCUA_TEST_DIR/server.log" 2>&1 &
  echo $! > "$OPCUA_TEST_DIR/server.pid"

  local i
  for ((i = 0; i < 100; i++)); do
    (exec 3<>"/dev/tcp/localhost/$OPCUA_SERVER_PORT") 2>/dev/null && return 0
    sleep 0.1
  done

  echo "serverStart: no answer on port $OPCUA_SERVER_PORT after 10s - $OPCUA_TEST_DIR/server.log:" >&2
  tail -20 "$OPCUA_TEST_DIR/server.log" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# serverStop - stop the server serverStart started (by its pid)
#
serverStop() {
  local pidFile="$OPCUA_TEST_DIR/server.pid"
  [ -f "$pidFile" ] || return 0

  local pid i
  pid=$(cat "$pidFile")
  kill "$pid" 2>/dev/null
  for ((i = 0; i < 50; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  kill -9 "$pid" 2>/dev/null
  rm -f "$pidFile"
}


# -----------------------------------------------------------------------------
#
# deviceSet <node id> <JSON value> <ISO 8601 time> - the server's own value changes, as a device's would,
# its SourceTimestamp the time given (the observedAt it becomes)
#
deviceSet() {
  echo "set $1 $3 $2" > "$OPCUA_TEST_DIR/server.cmd"
}


# -----------------------------------------------------------------------------
#
# deviceAwaitWrite <node id> <value> [seconds] - wait until a client has written <value> to the node, then
# print the write as the server recorded it
#
deviceAwaitWrite() {
  local secs="${3:-10}" i

  for ((i = 0; i < secs * 10; i++)); do
    grep -qxF -- "$1 $2" "$OPCUA_TEST_DIR/server.writes" && { echo "$1 $2"; return 0; }
    sleep 0.1
  done

  echo "deviceAwaitWrite: no write of '$2' to '$1' after ${secs}s - the writes: $(cat "$OPCUA_TEST_DIR/server.writes")" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# deviceWrites - every write the server has recorded
#
deviceWrites() {
  cat "$OPCUA_TEST_DIR/server.writes"
}


# -----------------------------------------------------------------------------
#
# entityAwait <entityId> <attr> <text> [seconds] - wait until GET the attribute (keyValues) contains <text>
#
entityAwait() {
  local url="localhost:$OPCUA_PORT/ngsi-ld/v1/entities/$1?options=keyValues&attrs=$2" secs="${4:-10}" i

  for ((i = 0; i < secs * 10; i++)); do
    curl -s "$url" | grep -qF -- "$3" && return 0
    sleep 0.1
  done

  echo "entityAwait: $1/$2 is $(curl -s "$url"), without '$3', after ${secs}s" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# entityGet <entityId> <attrs> [options] - the entity's attributes, JSON with sorted members
#
entityGet() {
  curl -s "localhost:$OPCUA_PORT/ngsi-ld/v1/entities/$1?attrs=$2${3:+&options=$3}" \
    | python3 -c 'import sys, json; print(json.dumps(json.load(sys.stdin), indent=2, sort_keys=True))'
}


# -----------------------------------------------------------------------------
#
# attrPatch <entityId> <attr> <body> - PATCH one attribute; prints the HTTP status
#
attrPatch() {
  curl -s -o /dev/null -w "%{http_code}\n" -X PATCH "localhost:$OPCUA_PORT/ngsi-ld/v1/entities/$1/attrs/$2" \
       -H 'Content-Type: application/json' -d "$3"
}


# -----------------------------------------------------------------------------
#
# assetChannels - one line per AssetChannel: endpoint, status, counters - and why, when it is dormant
#
assetChannels() {
  curl -s "localhost:$OPCUA_PORT/ngsi-ld/v1/assetChannels" | python3 -c 'import sys, json
for c in sorted(json.load(sys.stdin), key=lambda c: c["channelTarget"]):
  line = "%s %s modelsIn=%s valuesIn=%s valuesRefused=%s entitiesWritten=%s" % (c["channelTarget"], c["status"], c.get("modelsIn"), c.get("valuesIn"), c.get("valuesRefused"), c.get("entitiesWritten"))
  print(line if c["status"] == "available" else "%s - %s" % (line, c.get("statusReason")))'
}


# -----------------------------------------------------------------------------
#
# channels - one line per Channel: endpoint, status, counters - and why, when it is dormant
#
channels() {
  curl -s "localhost:$OPCUA_PORT/ngsi-ld/v1/channels" | python3 -c 'import sys, json
for c in sorted(json.load(sys.stdin), key=lambda c: c["channelTarget"]):
  line = "%s %s samplesIn=%s samplesOut=%s" % (c["channelTarget"], c["status"], c.get("samplesIn"), c.get("samplesOut"))
  print(line if c["status"] == "available" else "%s - %s" % (line, c.get("statusReason")))'
}
