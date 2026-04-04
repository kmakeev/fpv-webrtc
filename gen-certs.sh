#!/bin/bash
# gen-certs.sh — генерирует самоподписанный TLS сертификат для локальной сети
# Нужен для WebXR на Quest 2 (браузер требует HTTPS для immersive-vr)
#
# Использование:
#   chmod +x gen-certs.sh && ./gen-certs.sh
#
# Затем запуск сервера с TLS:
#   TLS=1 node server/server.js

set -e
mkdir -p server/certs

# Определяем IP ноутбука
if command -v hostname &>/dev/null; then
  LOCAL_IP=$(hostname -I 2>/dev/null | awk '{print $1}')
fi
LOCAL_IP=${LOCAL_IP:-"192.168.1.100"}

echo "Генерация сертификата для IP: ${LOCAL_IP}"

# Конфиг для Subject Alternative Names
cat > server/certs/openssl.cnf << EOF
[req]
default_bits       = 2048
prompt             = no
default_md         = sha256
distinguished_name = dn
x509_extensions    = v3_req

[dn]
CN = FPV-Server

[v3_req]
subjectAltName = @alt_names
keyUsage       = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth

[alt_names]
IP.1  = ${LOCAL_IP}
IP.2  = 127.0.0.1
DNS.1 = localhost
EOF

openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout server/certs/key.pem \
  -out    server/certs/cert.pem \
  -days   365 \
  -config server/certs/openssl.cnf

echo ""
echo "✓ Сертификат создан: server/certs/cert.pem"
echo ""
echo "Запуск с HTTPS:"
echo "  TLS=1 node server/server.js"
echo ""
echo "На Quest 2 зайдите на https://${LOCAL_IP}:8080"
echo "При первом открытии браузер покажет предупреждение — нажмите"
echo "'Дополнительно' → 'Перейти на сайт' для принятия самоподписанного сертификата."
