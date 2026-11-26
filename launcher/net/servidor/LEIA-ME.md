# Salas do Ranga num servidor (Azure)

Deixa o online ligado sem depender do PC do dono: Switch, 3DS, clássicos, System Link do Xbox e PS3. Perfis, chat e
saves continuam na central do Cloudflare, e a atualização, no GitHub. Se o servidor cair, o Ranga usa sozinho a sala do
PC do dono, pelo playit.

| Sala | Porta | Serviço |
|---|---|---|
| Switch | UDP 24872 | `ranga-switch` |
| Clássicos | TCP 24873 | `ranga-classicos` |
| 3DS | UDP 24874 | `ranga-3ds` |
| Xbox (System Link) | UDP 24875 | `ranga-classicos` |
| PS3 (RPCN) | TCP 31313 e UDP 3657 | `ranga-rpcn` |

## 1. Na Azure (o dono)

Confira o preço e o limite de horas grátis na tela antes de criar: máquina, disco e **IP público** são cobrados à parte.

- Máquina **Ubuntu Server 24.04 LTS, x64**, a menor com 1 GB serve. Região: Brazil South, se o crédito deixar.
- Entrada por **chave SSH** (nada de senha). IP público **estático**.
- Em **Networking → Inbound port rules**, abra as portas da tabela para qualquer origem, e a 22 só para o seu IP.
- Crie um alerta de orçamento baixo em **Cost Management → Budgets**.

## 2. O pacote e a instalação

No PC do dono, no Git Bash, e depois pelo SSH:

```
bash launcher/net/servidor/empacotar.sh
scp ranga-servidor.tar.gz <usuario>@<ip>:
ssh <usuario>@<ip> "tar xzf ranga-servidor.tar.gz && sudo bash ranga-servidor/instalar.sh"
```

No fim aparece `ok` para cada sala. Atualizar é repetir os mesmos comandos: as contas do PS3 ficam.

## 3. Avisar o Ranga de todos

Depois que **todos estiverem na 1.8** (quem está antes continua indo para o playit e não encontra os outros):

1. Testar cada sala com um amigo, pondo o endereço à mão.
2. No PC do dono: `RangaEmu.exe --servidor-salas <nome ou ip>`. O resultado sai em `salas\publicar.log`.
3. Para desfazer: `RangaEmu.exe --servidor-salas nenhum`. Todos voltam ao playit.

## Cópia das contas do PS3

Uma por dia em `/var/lib/ranga-rpcn/copias` (as 14 últimas). Estão na mesma máquina: baixe uma de vez em quando, e
guarde só no seu PC, porque ela leva as contas e a chave do certificado.

```
ssh <usuario>@<ip> "sudo systemctl start ranga-copia && sudo sh -c 'cat \$(ls -1t /var/lib/ranga-rpcn/copias/*.gz | head -1)'" > rpcn-copia.tar.gz
```

Para restaurar: `sudo systemctl stop ranga-rpcn`, abrir o arquivo em `/var/lib/ranga-rpcn`,
`sudo chown -R ranga-rpcn: /var/lib/ranga-rpcn`, `sudo systemctl start ranga-rpcn`.

## Ver e mexer

- Ver uma sala: `journalctl -u ranga-switch -f`. Estado: `systemctl status 'ranga-*'`.
- Nome e limite: `/etc/ranga/salas.conf`. Senha da sala do Switch: `/etc/ranga/senha-switch.conf`.
- A sala não tem senha por padrão: quem souber o endereço entra.
