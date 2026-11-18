#!/usr/bin/env bash
# Monta (ou remonta) a central do Ranga numa máquina Ubuntu, como a grátis da Oracle Cloud (ARM ou x86).
#
#   sudo bash instalar.sh <codigo-do-grupo> [porta]
#
# Precisa de RangaSala.cpp e RangaNet.hpp na mesma pasta deste script. Compila o servidor da sala, cria o usuário
# "ranga" sem login, guarda os dados em /var/lib/ranga (perfis, chat, atualização), liga o serviço para subir sozinho
# quando a máquina reiniciar e abre a porta no firewall da própria máquina. A porta também precisa estar aberta na
# "Security List" da rede, no site da Oracle (isso o script não alcança).
#
# Pode rodar de novo quantas vezes precisar: atualiza o programa e mantém os dados.
set -euo pipefail

GRUPO="${1:?Uso: sudo bash instalar.sh <codigo-do-grupo> [porta]}"
PORTA="${2:-24873}"
PASTA="$(cd "$(dirname "$0")" && pwd)"

if [[ ! "$GRUPO" =~ ^[A-Za-z0-9_-]+$ ]]; then
    echo "O código do grupo só pode ter letras, números, - e _." >&2
    exit 1
fi
if [[ $EUID -ne 0 ]]; then
    echo "Rode com sudo." >&2
    exit 1
fi

echo "== Compilador"
export DEBIAN_FRONTEND=noninteractive
apt-get update -y -q
apt-get install -y -q g++ iptables-persistent

echo "== Servidor da sala"
g++ -O2 -std=c++17 -Wall -o /usr/local/bin/ranga-sala.novo "$PASTA/RangaSala.cpp" -I "$PASTA"
mv -f /usr/local/bin/ranga-sala.novo /usr/local/bin/ranga-sala

echo "== Usuário e dados"
id ranga >/dev/null 2>&1 || useradd --system --home-dir /var/lib/ranga --create-home --shell /usr/sbin/nologin ranga
install -d -o ranga -g ranga -m 750 /var/lib/ranga /var/lib/ranga/perfis /var/lib/ranga/atualizacao

echo "== Serviço"
cat > /etc/systemd/system/ranga-sala.service <<EOF
[Unit]
Description=Central do Ranga (perfis, chat, atualizações e online dos clássicos)
After=network-online.target
Wants=network-online.target

[Service]
User=ranga
Group=ranga
ExecStart=/usr/local/bin/ranga-sala --port ${PORTA} --lan --perfis /var/lib/ranga/perfis --atualizacao /var/lib/ranga/atualizacao --grupo ${GRUPO} --log /var/lib/ranga/sala.log
Restart=always
RestartSec=3
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
ReadWritePaths=/var/lib/ranga

[Install]
WantedBy=multi-user.target
EOF
chmod 600 /etc/systemd/system/ranga-sala.service
systemctl daemon-reload
systemctl enable ranga-sala >/dev/null
systemctl restart ranga-sala

echo "== Firewall da máquina (porta TCP ${PORTA})"
# As imagens Ubuntu da Oracle recusam tudo menos SSH; a regra vai antes da recusa.
iptables -C INPUT -p tcp --dport "${PORTA}" -j ACCEPT 2>/dev/null || iptables -I INPUT 1 -p tcp --dport "${PORTA}" -j ACCEPT
netfilter-persistent save >/dev/null

sleep 1
if systemctl is-active --quiet ranga-sala; then
    echo "== Pronto: a central do Ranga está no ar na porta TCP ${PORTA}."
    tail -n 3 /var/lib/ranga/sala.log || true
else
    echo "== O serviço não subiu. Veja: journalctl -u ranga-sala -n 50" >&2
    exit 1
fi
