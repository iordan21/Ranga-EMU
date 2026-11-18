-- O banco D1 da central do Ranga (src/index.js).
CREATE TABLE IF NOT EXISTS perfis (
  id TEXT PRIMARY KEY,
  segredo TEXT NOT NULL,
  nome TEXT NOT NULL DEFAULT '',
  cartao TEXT NOT NULL,
  visto INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS chat (
  numero INTEGER PRIMARY KEY AUTOINCREMENT,
  quando INTEGER NOT NULL,
  id TEXT NOT NULL,
  nome TEXT NOT NULL,
  texto TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS chat_por_id ON chat (id, quando);
