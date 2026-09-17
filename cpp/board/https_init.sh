#!/bin/sh
# 自签 HTTPS 证书：capp 只读 cert.pem/key.pem，缺一就生成（由 init.sh 在起 capp 前调用）。
#
# 用 EC(prime256v1) 而不是原来的 RSA-4096：板上实测 RSA-4096 生成要 **64 秒**
# （小核 + 熵不够），而这一步是**卡在 capp 启动之前**跑的 —— 每次重新生成就等于
# 晚上线一分钟；EC 在板上实测 1 秒。capp 侧走 mbedTLS，ECDHE-ECDSA 套件是开的
# （实测握手 TLSv1.3 / TLS_AES_256_GCM_SHA384 正常）。
# 万一哪个环境的 openssl 没编 EC，退回 RSA-2048（也比 4096 快得多）。

AKA_HOME="${AKA_HOME:-$HOME/AKA-00}"
KEY_PEM="$AKA_HOME/key.pem"
CERT_PEM="$AKA_HOME/cert.pem"

# 已经有了就别动：换证书会让浏览器重新报"不受信任"
if [ -f "$KEY_PEM" ] && [ -f "$CERT_PEM" ]; then
    exit 0
fi

SUBJ="/C=CN/ST=Beijing/L=Beijing/O=MyOrg/OU=MyDept/CN=localhost"

if ! openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "$KEY_PEM" -out "$CERT_PEM" -days 3650 -nodes -subj "$SUBJ" 2>/dev/null; then
    echo "[https_init] EC 证书生成失败，退回 RSA-2048" >&2
    openssl req -x509 -newkey rsa:2048 -keyout "$KEY_PEM" -out "$CERT_PEM" \
        -days 3650 -nodes -subj "$SUBJ"
fi
