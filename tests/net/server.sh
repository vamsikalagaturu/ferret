#!/usr/bin/env bash
# SSH (SFTP), Samba, FTP/FTPS (pure-ftpd), WebDAV (Apache, http and https) and NFSv4 (nfs-ganesha) servers in a
# container on 127.0.0.1, for tests/test_remote.c:
#   tests/net/server.sh start     builds the image once (per Dockerfile) and starts it; prints the environment
#                                 test_remote wants
#   tests/net/server.sh restart   restarts it, dropping every connection (FR_TEST_RESTART for test_remote)
#   tests/net/server.sh stop
# Ports: FR_NET_SSH_PORT (2222), FR_NET_SMB_PORT (4450), FR_NET_FTP_PORT (2121, passive 30000-30009),
# FR_NET_DAV_PORT (8080), FR_NET_DAVS_PORT (8443), FR_NET_NFS_PORT (12049). User "ferret", password "ferret-test".
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
NAME=ferret-net-test
IMAGE=$NAME:$(cat "$HERE/Dockerfile" "$HERE/start.sh" | sha256sum | cut -c1-12)
SSH_PORT=${FR_NET_SSH_PORT:-2222}
SMB_PORT=${FR_NET_SMB_PORT:-4450}
FTP_PORT=${FR_NET_FTP_PORT:-2121}
DAV_PORT=${FR_NET_DAV_PORT:-8080}
DAVS_PORT=${FR_NET_DAVS_PORT:-8443}
NFS_PORT=${FR_NET_NFS_PORT:-12049}
ENGINE=$(command -v docker || command -v podman || true)
[ -n "$ENGINE" ] || { echo "missing: docker or podman" >&2; exit 1; }

up() { (exec 3<>/dev/tcp/127.0.0.1/$1) 2>/dev/null; }

wait_ports() {
    for _ in $(seq 100); do
        up $SSH_PORT && up $SMB_PORT && up $FTP_PORT && up $DAV_PORT && up $DAVS_PORT && return
        sleep 0.2
    done
}

case "${1:-}" in
start)
    "$ENGINE" image inspect $IMAGE >/dev/null 2>&1 || "$ENGINE" build -t $IMAGE "$HERE" >&2
    "$ENGINE" rm -f $NAME >/dev/null 2>&1 || true
    # pure-ftpd keeps CAP_SYS_NICE and CAP_DAC_READ_SEARCH for its sessions; nfs-ganesha needs the latter and a file
    # system with file handles (tmpfs, not the image's overlay)
    "$ENGINE" run -d --rm --name $NAME --cap-add DAC_READ_SEARCH --cap-add SYS_NICE --cap-add SYS_RESOURCE \
        --mount type=tmpfs,dst=/srv/nfs,tmpfs-mode=1777 \
        -p 127.0.0.1:$SSH_PORT:22 -p 127.0.0.1:$SMB_PORT:445 -p 127.0.0.1:$FTP_PORT:21 \
        -p 127.0.0.1:30000-30009:30000-30009 -p 127.0.0.1:$DAV_PORT:8080 -p 127.0.0.1:$DAVS_PORT:8443 \
        -p 127.0.0.1:$NFS_PORT:2049 $IMAGE >/dev/null
    wait_ports
    echo "export FR_TEST_SFTP=sftp://ferret@127.0.0.1:$SSH_PORT/home/ferret"
    echo "export FR_TEST_SMB=smb://ferret@127.0.0.1:$SMB_PORT/data"
    echo "export FR_TEST_FTP=ftp://ferret@127.0.0.1:$FTP_PORT/home/ferret"
    echo "export FR_TEST_FTPS=ftps://ferret@127.0.0.1:$FTP_PORT/home/ferret"
    echo "export FR_TEST_DAV=dav://ferret@127.0.0.1:$DAV_PORT/"
    echo "export FR_TEST_DAVS=davs://ferret@127.0.0.1:$DAVS_PORT/"
    for _ in $(seq 25); do up $NFS_PORT && break; sleep 0.2; done
    up $NFS_PORT && echo "export FR_TEST_NFS=nfs://127.0.0.1:$NFS_PORT/srv/nfs"
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
