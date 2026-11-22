-- O banco D1 da central do Ranga (src/index.js): contas, sessões, amizades e conversas.

-- Uma conta: nome#tag (a tag é de 4 dígitos, única para o nome), a senha só como PBKDF2-SHA256 com sal, o código de
-- recuperação também só como hash, e o cartão do perfil (o mesmo texto que o Ranga monta em PlayerProfile.cpp).
CREATE TABLE IF NOT EXISTS contas (
  id TEXT PRIMARY KEY,
  nome TEXT NOT NULL,
  nome_busca TEXT NOT NULL,
  tag INTEGER NOT NULL,
  senha TEXT NOT NULL,
  recuperacao TEXT NOT NULL,
  criada INTEGER NOT NULL,
  cartao TEXT NOT NULL DEFAULT '',
  visto INTEGER NOT NULL DEFAULT 0,
  pc TEXT NOT NULL DEFAULT '',
  banida INTEGER NOT NULL DEFAULT 0,
  motivo TEXT NOT NULL DEFAULT ''
);
CREATE UNIQUE INDEX IF NOT EXISTS contas_nome_tag ON contas (nome_busca, tag);
-- O PC que criou a conta (no máximo 3 por PC): o código que o Ranga manda, embaralhado de novo com o segredo PC_SAL.
CREATE INDEX IF NOT EXISTS contas_pc ON contas (pc);

-- Uma sessão aberta num PC: só o hash da chave que o Ranga guarda.
CREATE TABLE IF NOT EXISTS sessoes (
  chave TEXT PRIMARY KEY,
  conta TEXT NOT NULL,
  criada INTEGER NOT NULL,
  usada INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS sessoes_conta ON sessoes (conta);

-- Uma amizade ou um pedido: a e b em ordem (a < b), quem pediu, e se já foi aceito.
CREATE TABLE IF NOT EXISTS amizades (
  a TEXT NOT NULL,
  b TEXT NOT NULL,
  de TEXT NOT NULL,
  aceita INTEGER NOT NULL DEFAULT 0,
  criada INTEGER NOT NULL,
  PRIMARY KEY (a, b)
);
CREATE INDEX IF NOT EXISTS amizades_b ON amizades (b);

-- As conversas: cada mensagem de uma conta para outra (só entre amigos).
CREATE TABLE IF NOT EXISTS mensagens (
  numero INTEGER PRIMARY KEY AUTOINCREMENT,
  de TEXT NOT NULL,
  para TEXT NOT NULL,
  quando INTEGER NOT NULL,
  texto TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS mensagens_para ON mensagens (para, numero);
CREATE INDEX IF NOT EXISTS mensagens_de ON mensagens (de, numero);

-- Limites de tentativas (criar conta, entrar, recuperar) por IP ou por conta, em janelas de tempo.
CREATE TABLE IF NOT EXISTS limites (
  chave TEXT PRIMARY KEY,
  janela INTEGER NOT NULL,
  contagem INTEGER NOT NULL
);

-- Quem cada conta bloqueou: o bloqueado não pede amizade nem fala mais com ela.
CREATE TABLE IF NOT EXISTS bloqueios (
  quem TEXT NOT NULL,
  bloqueado TEXT NOT NULL,
  quando INTEGER NOT NULL,
  PRIMARY KEY (quem, bloqueado)
);

-- Os PCs de contas banidas pelo dono (o mesmo hash da coluna contas.pc): não criam outra conta.
CREATE TABLE IF NOT EXISTS pcs_banidos (
  pc TEXT PRIMARY KEY,
  quando INTEGER NOT NULL
);

-- Os desafios da prova de trabalho já usados (cada um vale uma vez; os velhos saem sozinhos).
CREATE TABLE IF NOT EXISTS desafios (
  desafio TEXT PRIMARY KEY,
  quando INTEGER NOT NULL
);

-- A foto e a capa de cada conta (o "RANI" do Ranga em base64) e a versão, que vai nos cartões dos amigos.
CREATE TABLE IF NOT EXISTS midia (
  conta TEXT NOT NULL,
  tipo TEXT NOT NULL,
  versao TEXT NOT NULL,
  dados TEXT NOT NULL,
  PRIMARY KEY (conta, tipo)
);

-- O estado de cada conversa visto por uma conta: até quando ela está digitando para a outra e até que mensagem da
-- outra ela já leu (o "visto" e o "digitando..." do chat).
CREATE TABLE IF NOT EXISTS conversas (
  conta TEXT NOT NULL,
  com TEXT NOT NULL,
  digitando INTEGER NOT NULL DEFAULT 0,
  lido INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (conta, com)
);
CREATE INDEX IF NOT EXISTS conversas_com ON conversas (com);

-- As imagens mandadas no chat (o "RANI" do Ranga em base64), só para quem mandou e quem recebeu; saem após 30 dias.
CREATE TABLE IF NOT EXISTS imagens (
  id TEXT PRIMARY KEY,
  de TEXT NOT NULL,
  para TEXT NOT NULL,
  quando INTEGER NOT NULL,
  dados TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS imagens_quando ON imagens (quando);

-- Os grupos de conversa (até 16 pessoas): quem criou manda no nome e em quem sai.
CREATE TABLE IF NOT EXISTS grupos (
  id TEXT PRIMARY KEY,
  nome TEXT NOT NULL,
  dono TEXT NOT NULL,
  criado INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS grupo_membros (
  grupo TEXT NOT NULL,
  conta TEXT NOT NULL,
  entrou INTEGER NOT NULL,
  PRIMARY KEY (grupo, conta)
);
CREATE INDEX IF NOT EXISTS grupo_membros_conta ON grupo_membros (conta);
CREATE TABLE IF NOT EXISTS grupo_mensagens (
  numero INTEGER PRIMARY KEY AUTOINCREMENT,
  grupo TEXT NOT NULL,
  de TEXT NOT NULL,
  quando INTEGER NOT NULL,
  texto TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS grupo_mensagens_grupo ON grupo_mensagens (grupo, numero);

-- As denúncias: quem denunciou, contra quem, o motivo e as últimas mensagens entre os dois, para o dono ver.
CREATE TABLE IF NOT EXISTS denuncias (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  de TEXT NOT NULL,
  contra TEXT NOT NULL,
  quando INTEGER NOT NULL,
  motivo TEXT NOT NULL,
  trecho TEXT NOT NULL,
  resolvida INTEGER NOT NULL DEFAULT 0
);

-- O save na nuvem: cada arquivo de save de uma conta (o caminho relativo à pasta do Ranga), a versão (hash) e de que
-- PC veio, e os pedaços dele (comprimido, em base64). Um arquivo novo só vale quando todos os pedaços chegaram.
CREATE TABLE IF NOT EXISTS nuvem_arquivos (
  conta TEXT NOT NULL,
  caminho TEXT NOT NULL,
  hash TEXT NOT NULL,
  tamanho INTEGER NOT NULL,
  bruto INTEGER NOT NULL,
  partes INTEGER NOT NULL,
  quando INTEGER NOT NULL,
  pc TEXT NOT NULL,
  PRIMARY KEY (conta, caminho)
);
CREATE TABLE IF NOT EXISTS nuvem_partes (
  conta TEXT NOT NULL,
  hash TEXT NOT NULL,
  indice INTEGER NOT NULL,
  dados TEXT NOT NULL,
  quando INTEGER NOT NULL,
  PRIMARY KEY (conta, hash, indice)
);
