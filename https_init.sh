#!/bin/sh

AKA_HOME="${AKA_HOME:-$HOME/AKA-00}"
KEY_PEM="$AKA_HOME/key.pem"
CERT_PEM="$AKA_HOME/cert.pem"

if ! ( [ -f "$KEY_PEM" ] && [ -f "$CERT_PEM" ] ) ; then
  openssl req -x509 -newkey rsa:4096 -keyout "$KEY_PEM" -out "$CERT_PEM" -days 3650 -nodes -subj "/C=CN/ST=Beijing/L=Beijing/O=MyOrg/OU=MyDept/CN=localhost"
fi