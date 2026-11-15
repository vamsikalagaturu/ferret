#!/usr/bin/env bash
# An SSH (SFTP) and Samba server in a container on 127.0.0.1, for tests/test_remote.c:
#   tests/net/server.sh start     builds the image once and starts it; prints the environment test_remote wants
#   tests/net/server.sh restart   restarts it, dropping every connection (FR_TEST_RESTART for test_remote)
#   tests/net/server.sh stop
# Ports: FR_NET_SSH_PORT (2222) and FR_NET_SMB_PORT (4450). User "ferret", password "ferret-test".
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
NAME=ferret-net-test
SSH_PORT=${FR_NET_SSH_PORT:-2222}
SMB_PORT=${FR_NET_SMB_PORT:-4450}
ENGINE=$(command -v docker || command -v podman || true)
[ -n "$ENGINE" ] || { echo "missing: docker or podman" >&2; exit 1; }

wait_ports() {
    for _ in $(seq 50); do
        (exec 3<>/dev/tcp/127.0.0.1/$SSH_PORT) 2>/dev/null && (exec 3<>/dev/tcp/127.0.0.1/$SMB_PORT) 2>/dev/null && return
        sleep 0.2
    done
}

case "${1:-}" in
start)
    "$ENGINE" image inspect $NAME >/dev/null 2>&1 || "$ENGINE" build -t $NAME "$HERE" >&2
    "$ENGINE" rm -f $NAME >/dev/null 2>&1 || true
    "$ENGINE" run -d --rm --name $NAME -p 127.0.0.1:$SSH_PORT:22 -p 127.0.0.1:$SMB_PORT:445 $NAME >/dev/null
    wait_ports
    echo "export FR_TEST_SFTP=sftp://ferret@127.0.0.1:$SSH_PORT/home/ferret"
    echo "export FR_TEST_SMB=smb://ferret@127.0.0.1:$SMB_PORT/data"
    echo "export FR_TEST_PASSWORD=ferret-test"
    echo "export FR_TEST_RESTART='$HERE/server.sh restart'"
    ;;
restart)
    "$ENGINE" restart -t 0 $NAME >/dev/null
    wait_ports
    sleep 1
    ;;
stop) "$ENGINE" rm -f $NAME >/dev/null ;;
*) echo "usage: $0 start | restart | stop" >&2; exit 1 ;;
esac
