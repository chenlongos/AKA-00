#!/bin/sh

KEY_PEM="/root/AKA-00/key.pem"
CERT_PEM="/root/AKA-00/cert.pem"

if ! ( [ -f "$KEY_PEM" ] && [ -f "$CERT_PEM" ] ) ; then
  openssl req -x509 -newkey rsa:4096 -keyout /root/AKA-00/key.pem -out /root/AKA-00/cert.pem -days 3650 -nodes -subj "/C=CN/ST=Beijing/L=Beijing/O=MyOrg/OU=MyDept/CN=localhost"
fi