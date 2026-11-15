FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends openssh-server samba \
    pure-ftpd apache2 apache2-utils openssl nfs-ganesha nfs-ganesha-vfs rpcbind && rm -rf /var/lib/apt/lists/*
RUN useradd -m -s /bin/bash ferret && echo 'ferret:ferret-test' | chpasswd && (printf 'ferret-test\nferret-test\n' | smbpasswd -a -s ferret) \
 && mkdir -p /srv/data /run/sshd && chown ferret:ferret /srv/data \
 && printf '[global]\n server min protocol = SMB2_02\n map to guest = never\n[data]\n path = /srv/data\n read only = no\n valid users = ferret\n' > /etc/samba/smb.conf \
 && printf 'PasswordAuthentication yes\nKbdInteractiveAuthentication no\n' > /etc/ssh/sshd_config.d/test.conf
# one self-signed certificate for FTPS and WebDAV over https, so the tests meet an untrusted one
RUN openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj /CN=ferret-test -keyout /etc/ssl/private/test.key \
    -out /etc/ssl/certs/test.crt && cat /etc/ssl/private/test.key /etc/ssl/certs/test.crt > /etc/ssl/private/pure-ftpd.pem \
 && chmod 600 /etc/ssl/private/pure-ftpd.pem
# WebDAV on 8080 (http) and 8443 (https), user ferret
RUN a2enmod dav dav_fs ssl auth_basic >/dev/null && mkdir -p /srv/dav /var/lib/apache2/dav \
 && chown www-data:www-data /srv/dav /var/lib/apache2/dav \
 && htpasswd -bc /etc/apache2/dav.passwd ferret ferret-test \
 && printf 'Listen 8080\nListen 8443\nDavLockDB /var/lib/apache2/dav/lock\n<Directory /srv/dav>\n Dav On\n AuthType Basic\n AuthName dav\n AuthUserFile /etc/apache2/dav.passwd\n Require valid-user\n</Directory>\n<VirtualHost *:8080>\n DocumentRoot /srv/dav\n</VirtualHost>\n<VirtualHost *:8443>\n DocumentRoot /srv/dav\n SSLEngine on\n SSLCertificateFile /etc/ssl/certs/test.crt\n SSLCertificateKeyFile /etc/ssl/private/test.key\n</VirtualHost>\n' > /etc/apache2/sites-enabled/dav.conf \
 && printf '' > /etc/apache2/ports.conf && rm -f /etc/apache2/sites-enabled/000-default.conf
# NFSv4 (userspace, nfs-ganesha) exporting /srv/nfs to everyone, writable; it registers with rpcbind even for v4
RUN mkdir -p /srv/nfs && chmod 1777 /srv/nfs \
 && printf 'NFS_CORE_PARAM { Protocols = 4; Enable_RQUOTA = false; Enable_NLM = false; }\nNFSv4 { Graceless = true; }\nEXPORT { Export_Id = 1; Path = /srv/nfs; Pseudo = /srv/nfs; Access_Type = RW; Squash = No_Root_Squash; Protocols = 4; Transports = TCP; SecType = sys; FSAL { Name = VFS; } }\n' > /etc/ganesha/ganesha.conf
COPY start.sh /start.sh
CMD ["/bin/sh", "/start.sh"]
