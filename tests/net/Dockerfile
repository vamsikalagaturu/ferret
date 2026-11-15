FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends openssh-server samba && rm -rf /var/lib/apt/lists/*
RUN useradd -m -s /bin/bash ferret && echo 'ferret:ferret-test' | chpasswd && (printf 'ferret-test\nferret-test\n' | smbpasswd -a -s ferret) \
 && mkdir -p /srv/data /run/sshd && chown ferret:ferret /srv/data \
 && printf '[global]\n server min protocol = SMB2_02\n map to guest = never\n[data]\n path = /srv/data\n read only = no\n valid users = ferret\n' > /etc/samba/smb.conf \
 && printf 'PasswordAuthentication yes\nKbdInteractiveAuthentication no\n' > /etc/ssh/sshd_config.d/test.conf
CMD ["/bin/sh", "-c", "smbd --foreground --no-process-group & exec /usr/sbin/sshd -D -e"]
