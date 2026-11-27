#!/usr/bin/env bash
# Monta (ou atualiza) as salas do Ranga numa máquina Ubuntu 24.04 x86-64, como a da Azure.
#
#   sudo bash instalar.sh            as quatro: Switch, 3DS, clássicos (com o hub do Xbox) e PS3 (RPCN)
#   sudo bash instalar.sh --sem-ps3  sem o RPCN
#
# Cada uma é um serviço: sobe com a máquina, volta sozinha se cair, roda sem permissão sobre o resto. O firewall da
# máquina libera só o SSH e as portas das salas (as mesmas precisam estar abertas na rede da Azure, ver LEIA-ME.md).
# Opções em /etc/ranga/salas.conf (nome, senha, limite). Pode rodar de novo: atualiza os programas e mantém os dados.
set -euo pipefail
PS3=1
for opcao in "$@"; do
    case "$opcao" in
        --sem-ps3) PS3=0 ;;
        *)
            echo "Uso: sudo bash instalar.sh [--sem-ps3]" >&2
            exit 2
            ;;
    esac
done
[[ $EUID -eq 0 ]] || { echo "Rode com sudo." >&2; exit 1; }
cd "$(dirname "$0")"

RPCN_VERSAO=1.12.1
RPCN_SHA256=73073d186f6a8597b7112e168712b578afcc1ed69906ffba948bb0743d4b2bb6
if [[ $PS3 -eq 1 && "$(uname -m)" != x86_64 ]]; then
    echo "O RPCN pronto é só para x86-64 e esta máquina é $(uname -m): sigo sem o PS3." >&2
    PS3=0
fi

echo "== Programas do Ubuntu"
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q g++ curl sqlite3 ufw unattended-upgrades

# Compilar pede uns 600 MB; uma máquina de 1 GB ganha 2 GB de troca, uma vez.
if [[ $(awk '/MemTotal/{print int($2/1024)}' /proc/meminfo) -lt 1900 && -z "$(swapon --show --noheadings)" && ! -e /swapfile-ranga ]]; then
    echo "== Memória de troca"
    fallocate -l 2G /swapfile-ranga
    chmod 600 /swapfile-ranga
    mkswap /swapfile-ranga >/dev/null
    swapon /swapfile-ranga
    grep -q swapfile-ranga /etc/fstab || echo "/swapfile-ranga none swap sw 0 0" >>/etc/fstab
fi

echo "== Salas"
bash compilar.sh ./prontos
# Um arquivo novo no lugar do velho não atrapalha o que está rodando; a troca vale no reinício, mais abaixo.
for nome in ranga-sala-switch ranga-sala-3ds ranga-sala; do
    install -m 755 "prontos/$nome" "/usr/local/bin/$nome.novo"
    mv -f "/usr/local/bin/$nome.novo" "/usr/local/bin/$nome"
done
install -m 755 copia-rpcn.sh /usr/local/sbin/ranga-copia-rpcn

echo "== Opções"
install -d -m 755 /etc/ranga
if [[ ! -f /etc/ranga/salas.conf ]]; then
    cat >/etc/ranga/salas.conf <<'EOF'
# Depois de mudar: sudo systemctl restart ranga-switch ranga-3ds
NOME_SWITCH="Sala do Ranga"
MAXIMO_SWITCH=16
NOME_3DS="Sala do Ranga (3DS)"
MAXIMO_3DS=8
EOF
fi
# A senha das salas (vazia: entra quem tiver o endereço), só para o administrador ler.
if [[ ! -f /etc/ranga/senha-switch.conf ]]; then
    (umask 077; echo 'RANGA_SENHA=""' >/etc/ranga/senha-switch.conf)
fi
# O código do grupo para as mesas dos clássicos, só para o administrador ler. MESAS_ARGS="--grupo CODIGO" guarda o
# código; com "--grupo CODIGO --mesas-com-codigo" só senta quem traz o código (Ranga 1.8.2 em diante, que o recebe da
# central). Vazio: as mesas aceitam quem tiver o endereço. Depois de mudar: sudo systemctl restart ranga-classicos
if [[ ! -f /etc/ranga/mesas.conf ]]; then
    (umask 077; echo 'MESAS_ARGS=' >/etc/ranga/mesas.conf)
fi

# servico nome descrição comando [linhas a mais da seção Service]
servico() {
    cat >"/etc/systemd/system/$1.service" <<EOF
[Unit]
Description=$2
After=network-online.target
Wants=network-online.target
StartLimitIntervalSec=0

[Service]
ExecStart=$3
Restart=always
RestartSec=3
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
PrivateDevices=true
ProtectKernelTunables=true
ProtectControlGroups=true
RestrictSUIDSGID=true
LockPersonality=true
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX AF_NETLINK
CapabilityBoundingSet=
MemoryMax=384M
TasksMax=128
${4:-}

[Install]
WantedBy=multi-user.target
EOF
}

echo "== Serviços"
servico ranga-switch "Sala do Ranga: Switch (UDP 24872)" \
    '/usr/local/bin/ranga-sala-switch --porta 24872 --nome ${NOME_SWITCH} --maximo ${MAXIMO_SWITCH}' \
    $'DynamicUser=yes\nEnvironmentFile=/etc/ranga/salas.conf\nEnvironmentFile=/etc/ranga/senha-switch.conf'
servico ranga-3ds "Sala do Ranga: 3DS (UDP 24874)" \
    '/usr/local/bin/ranga-sala-3ds --porta 24874 --nome ${NOME_3DS} --maximo ${MAXIMO_3DS}' \
    $'DynamicUser=yes\nEnvironmentFile=/etc/ranga/salas.conf'
# Sem --perfis e sem --atualizacao: perfis e chat são da central e a atualização é do GitHub; aqui é só o online.
servico ranga-classicos "Sala do Ranga: clássicos (TCP 24873) e hub do Xbox (UDP 24875)" \
    '/usr/local/bin/ranga-sala --port 24873 --lan --hub-xbox 24875 --log /var/lib/ranga-classicos/sala.log $MESAS_ARGS' \
    $'DynamicUser=yes\nStateDirectory=ranga-classicos\nEnvironmentFile=/etc/ranga/mesas.conf'
cat >/etc/logrotate.d/ranga <<'EOF'
/var/lib/ranga-classicos/sala.log {
    weekly
    rotate 4
    size 1M
    missingok
    notifempty
    copytruncate
    compress
}
EOF
SERVICOS=(ranga-switch ranga-3ds ranga-classicos)

if [[ $PS3 -eq 1 ]]; then
    echo "== Servidor do PS3 (RPCN $RPCN_VERSAO)"
    id ranga-rpcn >/dev/null 2>&1 || useradd --system --home-dir /var/lib/ranga-rpcn --shell /usr/sbin/nologin ranga-rpcn
    install -d -o ranga-rpcn -g ranga-rpcn -m 750 /var/lib/ranga-rpcn
    install -d -o ranga-rpcn -g ranga-rpcn -m 700 /var/lib/ranga-rpcn/copias /var/lib/ranga-rpcn/db
    if [[ "$(cat /usr/local/lib/ranga-rpcn/versao 2>/dev/null || true)" != "$RPCN_VERSAO" ]]; then
        T="$(mktemp -d)"
        trap 'rm -rf "$T"' EXIT
        curl -fsSL --retry 3 -o "$T/rpcn.tar.gz" "https://github.com/RipleyTom/rpcn/releases/download/$RPCN_VERSAO/rpcn-linux.tar.gz"
        echo "$RPCN_SHA256  $T/rpcn.tar.gz" | sha256sum -c --quiet -
        mkdir "$T/aberto"
        tar -C "$T/aberto" -xzf "$T/rpcn.tar.gz"
        ONDE="$(dirname "$(find "$T/aberto" -type f -name rpcn | head -1)")"
        [[ -f "$ONDE/rpcn" ]] || { echo "O pacote do RPCN veio sem o programa." >&2; exit 1; }
        FALTA="$(ldd "$ONDE/rpcn" 2>&1 || true)"
        if [[ "$FALTA" == *"not found"* ]]; then
            printf 'O RPCN não roda neste Ubuntu:
%s
' "$FALTA" >&2
            exit 1
        fi
        # Uma cópia das contas antes de trocar a versão.
        [[ -d /var/lib/ranga-rpcn/db ]] && runuser -u ranga-rpcn -- /usr/local/sbin/ranga-copia-rpcn
        install -d -m 755 /usr/local/lib/ranga-rpcn
        install -m 755 "$ONDE/rpcn" /usr/local/lib/ranga-rpcn/rpcn.novo
        mv -f /usr/local/lib/ranga-rpcn/rpcn.novo /usr/local/lib/ranga-rpcn/rpcn
        for f in "$ONDE"/*.cfg; do
            [[ -f "/var/lib/ranga-rpcn/$(basename "$f")" ]] || install -o ranga-rpcn -g ranga-rpcn -m 640 "$f" /var/lib/ranga-rpcn/
        done
        echo "$RPCN_VERSAO" >/usr/local/lib/ranga-rpcn/versao
    fi
    # Como no PC do dono: aberto para fora, sem e-mail (cada PS3 cria a própria conta), porta 31313.
    sed -i -E 's/^Host=.*/Host=0.0.0.0/; s/^Port=.*/Port=31313/; s/^EmailUrl=.*/EmailUrl=/; s/^StatServer=.*/StatServer=false/; s/^Verbosity=.*/Verbosity=Info/' /var/lib/ranga-rpcn/rpcn.cfg
    if [[ ! -f /var/lib/ranga-rpcn/cert.pem || ! -f /var/lib/ranga-rpcn/key.pem ]]; then
        (cd /var/lib/ranga-rpcn && runuser -u ranga-rpcn -- /usr/local/lib/ranga-rpcn/rpcn --cert-gen) >/dev/null
        chmod 600 /var/lib/ranga-rpcn/key.pem
    fi
    servico ranga-rpcn "Sala do Ranga: PS3, RPCN (TCP 31313, UDP 3657)" /usr/local/lib/ranga-rpcn/rpcn \
        $'User=ranga-rpcn\nGroup=ranga-rpcn\nWorkingDirectory=/var/lib/ranga-rpcn\nReadWritePaths=/var/lib/ranga-rpcn'
    cat >/etc/systemd/system/ranga-copia.service <<'EOF'
[Unit]
Description=Cópia das contas do servidor do PS3 (Ranga)

[Service]
Type=oneshot
User=ranga-rpcn
Group=ranga-rpcn
ExecStart=/usr/local/sbin/ranga-copia-rpcn
NoNewPrivileges=true
ProtectSystem=strict
ReadWritePaths=/var/lib/ranga-rpcn
EOF
    cat >/etc/systemd/system/ranga-copia.timer <<'EOF'
[Unit]
Description=Cópia diária das contas do servidor do PS3 (Ranga)

[Timer]
OnCalendar=*-*-* 08:00:00 UTC
Persistent=true

[Install]
WantedBy=timers.target
EOF
    SERVICOS+=(ranga-rpcn)
fi

echo "== Firewall"
# O SSH primeiro, na porta em que ele está: ligar o firewall sem ele trancaria a máquina para fora.
for porta in $(sshd -T 2>/dev/null | awk '$1=="port"{print $2}' | sort -u | grep . || echo 22); do ufw allow "$porta/tcp" >/dev/null; done
ufw allow 24872/udp >/dev/null
ufw allow 24873/tcp >/dev/null
ufw allow 24874/udp >/dev/null
ufw allow 24875/udp >/dev/null
if [[ $PS3 -eq 1 ]]; then
    ufw allow 31313/tcp >/dev/null
    ufw allow 3657/udp >/dev/null
fi
ufw default deny incoming >/dev/null
ufw default allow outgoing >/dev/null
ufw --force enable >/dev/null

systemctl daemon-reload
systemctl enable "${SERVICOS[@]}" >/dev/null 2>&1
systemctl restart "${SERVICOS[@]}"
if [[ $PS3 -eq 1 ]]; then systemctl enable --now ranga-copia.timer >/dev/null 2>&1; fi

sleep 4
RUIM=0
escuta() { ss -Hln"$1" "sport = :$2" | grep -q .; }
conferir() { # serviço tipo porta
    if systemctl is-active --quiet "$1" && escuta "$2" "$3"; then
        echo "   ok       $1 (porta $3 $([[ $2 == t ]] && echo TCP || echo UDP))"
    else
        echo "   PROBLEMA $1: veja journalctl -u $1 -n 40" >&2
        RUIM=1
    fi
}
echo "== Conferência"
conferir ranga-switch u 24872
conferir ranga-3ds u 24874
conferir ranga-classicos t 24873
conferir ranga-classicos u 24875
if [[ $PS3 -eq 1 ]]; then
    conferir ranga-rpcn t 31313
    conferir ranga-rpcn u 3657
fi
[[ $RUIM -eq 0 ]] && echo "Pronto. Falta só conferir as portas na rede da Azure (LEIA-ME.md)."
exit $RUIM
