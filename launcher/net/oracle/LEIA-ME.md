# Central do Ranga na Oracle Cloud (grátis)

Um servidor sempre ligado para os perfis, o chat, as atualizações e o online dos clássicos. Quando ele não responde,
o Ranga usa sozinho a sala do PC que hospeda, pelo playit, como antes. Nada fica só lá: perfis, conquistas e saves
continuam em cada PC.

## 1. Conta (quem faz é você)

1. Abra **oracle.com/cloud/free** e clique em **Start for free**.
2. País **Brazil**, nome e e-mail. Confirme o e-mail.
3. Crie a senha e um nome de conta (ex.: `rangaemu`).
4. **Home Region: Brazil East (São Paulo).** Não dá para trocar depois.
5. Endereço, telefone e cartão. O cartão é só verificação: a conta grátis não cria nada pago.
6. Espere a conta ficar pronta (alguns minutos, chega um e-mail).

## 2. A máquina

1. Menu ☰ → **Compute** → **Instances** → **Create instance**.
2. Nome: `ranga-central`.
3. **Image**: troque para **Canonical Ubuntu 24.04**.
4. **Shape**: **Ampere** → `VM.Standard.A1.Flex` com **1 OCPU e 6 GB** (tem o selo *Always Free-eligible*).
   Se aparecer *Out of capacity*, tente mais tarde ou use a AMD `VM.Standard.E2.1.Micro`, também grátis.
5. **Networking**: rede nova (VCN), sub-rede pública, **Assign a public IPv4 address** marcado.
6. **Add SSH keys** → **Paste public keys** → cole a chave pública do Ranga (`%USERPROFILE%\.ssh\ranga-oracle.pub`).
7. **Create**. Quando ficar **Running**, copie o **Public IP address**.

## 3. A porta

Na página da máquina: clique na **Subnet** → **Security Lists** → **Default Security List** → **Add Ingress Rules**:

- Source CIDR: `0.0.0.0/0`
- IP Protocol: `TCP`
- Destination Port Range: `24873`

## 4. A instalação (eu faço pelo SSH)

```
ssh -i ~/.ssh/ranga-oracle ubuntu@<IP> "mkdir -p ranga"
scp -i ~/.ssh/ranga-oracle launcher/net/oracle/instalar.sh launcher/net/RangaSala.cpp launcher/net/RangaNet.hpp ubuntu@<IP>:ranga/
ssh -i ~/.ssh/ranga-oracle ubuntu@<IP> "sudo bash ranga/instalar.sh <codigo-do-grupo>"
```

Depois, no Ranga de cada um: **Perfil → Amigos → Servidor dos amigos**: central `<IP>:24873` e o código do grupo.

## Se a Oracle apagar a máquina

Crie outra (passos 2 e 3) e rode a instalação de novo. Os perfis voltam sozinhos (cada Ranga manda o seu); só o
histórico do chat que estava lá se perde.
