#!/bin/sh
# The servers of tests/net/Dockerfile; the container lives as long as sshd.
smbd --foreground --no-process-group &
# explicit TLS allowed (-Y 1), no anonymous (-E), passive ports the host maps (-p), the address the host reaches (-P)
pure-ftpd -l unix -E -Y 1 -p 30000:30009 -P 127.0.0.1 -S 0.0.0.0,21 &
apache2ctl start
mkdir -p /var/run/ganesha
rpcbind -w
ganesha.nfsd -F -L /dev/stderr -f /etc/ganesha/ganesha.conf &
exec /usr/sbin/sshd -D -e
