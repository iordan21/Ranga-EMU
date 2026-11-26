#!/usr/bin/env bash
# A cópia de segurança do servidor do PS3 (contas, amizades, placares e o certificado), uma por dia, as 14 últimas, em
# /var/lib/ranga-rpcn/copias. O timer ranga-copia.timer chama; pode rodar à mão. O servidor não precisa parar: o banco é
# copiado pelo próprio SQLite, que espera uma gravação terminar.
set -euo pipefail
umask 077
BASE=/var/lib/ranga-rpcn
T="$(mktemp -d "$BASE/.copia.XXXXXX")"
trap 'rm -rf "$T"' EXIT
mkdir -p "$BASE/copias" "$T/db"
shopt -s nullglob
BANCOS=0
for banco in "$BASE"/db/*.db; do
    sqlite3 -readonly -cmd ".timeout 15000" "$banco" ".backup '$T/db/$(basename "$banco")'"
    [[ "$(sqlite3 "$T/db/$(basename "$banco")" 'PRAGMA integrity_check;')" == ok ]] || { echo "cópia com defeito: $banco" >&2; exit 1; }
    BANCOS=$((BANCOS + 1))
done
if [[ $BANCOS -eq 0 ]]; then
    echo "O servidor do PS3 ainda não tem banco: nada para copiar." >&2
    exit 0
fi
# O resto da pasta db (dados dos jogos salvos no servidor), que não é banco.
find "$BASE/db" -mindepth 1 -maxdepth 1 ! -name '*.db' ! -name '*.db-*' -exec cp -a {} "$T/db/" \;
# Placares e arquivos que os jogos guardam no servidor ficam fora do banco.
for d in score_data tus_data; do [[ -d "$BASE/$d" ]] && cp -a "$BASE/$d" "$T/"; done
for f in rpcn.cfg cert.pem key.pem; do [[ -f "$BASE/$f" ]] && cp -a "$BASE/$f" "$T/"; done
NOME="$BASE/copias/rpcn-$(date +%Y-%m-%d-%H%M%S).tar.gz"
tar -C "$T" -czf "$NOME.parte" .
mv "$NOME.parte" "$NOME"
ls -1t "$BASE"/copias/rpcn-*.tar.gz | tail -n +15 | xargs -r rm -f --
echo "Cópia pronta: $NOME"
