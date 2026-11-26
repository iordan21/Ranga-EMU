// A central do Ranga na Cloudflare (Workers + D1, plano grátis): contas, amigos, perfis e conversas, sempre no ar.
//
// Contas sem e-mail: nome#tag (a tag, de 4 dígitos, o servidor sorteia), a senha guardada só como PBKDF2-SHA256 com
// sal, e um código de recuperação (também só o hash) para quem esquecer a senha. Entrar dá uma chave de sessão
// aleatória; o servidor guarda só o hash dela. Tudo o mais pede "Authorization: Bearer <chave>".
//
// O formato é o do Ranga: linhas "campo<TAB>valor" (UTF-8), nos pedidos e nas respostas.
//
//   GET  /conta/desafio                                   -> desafio, bits (a prova de trabalho de criar conta)
//   POST /conta/criar      nome, senha, pc, desafio, resposta -> conta, nome, tag, sessao, recuperacao (no máximo 3 por PC)
//   POST /conta/entrar     nome (Nome#1234), senha        -> conta, nome, tag, sessao
//   POST /conta/recuperar  nome, recuperacao, senha       -> conta, nome, tag, sessao, recuperacao (um novo)
//   POST /conta/sair                                      (fecha esta sessão)
//   POST /conta/senha      senha (a atual), nova          (fecha as outras sessões)
//   POST /perfil           o cartão do perfil (PlayerProfile.cpp WriteCard)
//   GET  /amigos           amigos (com os cartões), pedidos recebidos e enviados
//   POST /amigos/pedir     nome (Nome#1234)               (aceita na hora se o outro já tinha pedido)
//   POST /amigos/responder conta, aceitar (1 ou 0)
//   POST /amigos/remover   conta
//   POST /mensagem         para (conta), texto            (só entre amigos; uma por segundo)
//   GET  /mensagens?desde=N                               as conversas desta conta depois da mensagem N, e depois
//                                                         delas "d<TAB>conta" (está digitando para mim) e
//                                                         "l<TAB>conta<TAB>N" (leu minhas mensagens até a N)
//   POST /digitando        para (conta)                   (vale por 6 segundos)
//   POST /lido             de (conta), numero             (li até essa mensagem dela)
//   POST /imagem           para (conta), dados (base64 do "RANI") -> id; a mensagem leva "[[imagem|id]]"
//   GET  /imagem?id=X                                     a imagem (só para quem mandou e quem recebeu)
//   POST /midia            tipo (foto ou capa), dados (base64 do "RANI"; vazio tira)
//   GET  /midia?conta=X&tipo=foto                         a foto ou a capa de um amigo (ou a própria)
//   POST /bloquear         conta                          (desfaz a amizade; o bloqueado não pede nem fala mais)
//   POST /desbloquear      conta
//   POST /denunciar        conta, motivo                  (vai para o dono com as últimas mensagens entre os dois)
//
// Grupos (até 16 pessoas; só se põe amigo seu). As mensagens deles vêm em GET /mensagens?desde=N&desdeg=M, como
// "g<TAB>numero<TAB>quando<TAB>grupo<TAB>de<TAB>texto" (um Ranga antigo ignora essas linhas):
//   GET  /grupos                                          "grupo<TAB>id<TAB>nome<TAB>dono" e "membro<TAB>grupo<TAB>conta<TAB>Nome#1234"
//   POST /grupo/criar      nome, membros (contas, separadas por vírgula) -> id
//   POST /grupo/adicionar  grupo, conta                   (qualquer membro põe um amigo seu)
//   POST /grupo/remover    grupo, conta                   (só quem criou)
//   POST /grupo/renomear   grupo, nome                    (só quem criou)
//   POST /grupo/sair       grupo                          (o grupo vazio some)
//   POST /grupo/mensagem   grupo, texto
//   POST /imagem           grupo, dados                   (a imagem vai para o grupo)
//
// Save na nuvem (no máximo NUVEM_MAX bytes comprimidos por conta):
//   GET  /nuvem                                           "a<TAB>caminho<TAB>hash<TAB>tamanho<TAB>bruto<TAB>partes<TAB>quando<TAB>pc"
//                                                         e "cota<TAB>usado<TAB>máximo"
//   POST /nuvem/parte      hash, indice, dados (base64, até NUVEM_PARTE)
//   POST /nuvem/fechar     caminho, hash, tamanho, bruto, partes, antes (o hash que o PC acha que a nuvem tem), pc
//                                                         -> 409 "conflito" se outro PC mandou outra versão antes
//   GET  /nuvem/parte?hash=X&indice=N                     um pedaço
//   POST /nuvem/limpar                                    apaga tudo desta conta na nuvem
//
// O dono do Ranga (cabeçalho X-Ranga-Dono com o segredo DONO_CHAVE):
//   GET  /dono/contas?busca=texto                         as contas (as mais novas primeiro)
//   POST /dono/banir       conta, motivo                  (fecha as sessões, desfaz as amizades, o PC dela não cria outra)
//   POST /dono/desbanir    conta
//   GET  /dono/denuncias                                  as denúncias abertas
//   POST /dono/denuncia    id                             (arquiva)
//   POST /dono/salas       servidor e as reservas         (o servidor das salas; GET /salas entrega a quem tem conta)

const PBKDF2_ITERATIONS = 20000;
const MAX_ACCOUNTS = 5000;
const ACCOUNTS_PER_PC = 3;
// Prova de trabalho: o Ranga acha um número que, junto do desafio, dá um SHA-256 começando com POW_BITS bits zero
// (uns segundos num PC comum). Uma conta custa isso; mil contas por programa custam mil vezes.
const POW_BITS = 23;
const POW_SECONDS = 600;
// O cartão do perfil tem poucos KB; nada além da foto e da capa precisa de mais.
const MAX_BODY = 64 * 1024;
// A foto ou a capa: o Ranga já manda com no máximo ~900 KB (Media.cpp), em base64.
const MAX_MEDIA = 1300 * 1024;
const MAX_TEXT = 500;
// Uma imagem do chat: o Ranga manda no máximo ~350 KB, em base64.
const MAX_IMAGE = 500 * 1024;
const IMAGE_DAYS = 30;
// Imagens por conta: 30 por hora, 100 por dia, e no máximo 40 MB guardados (os 30 dias) — o plano grátis tem cota.
const IMAGES_PER_DAY = 100;
const IMAGE_BYTES = 40 * 1024 * 1024;
const GROUP_MAX = 16;
const GROUPS_PER_ACCOUNT = 20;
// O save na nuvem: 64 MB comprimidos por conta (um cartão de memória vazio do PS2 vira quase nada), pedaços de até
// 700 KB em base64 (a D1 guarda até 2 MB por linha), um arquivo de até 24 MB comprimido.
const NUVEM_MAX = 64 * 1024 * 1024;
const NUVEM_PARTE = 700 * 1024;
const NUVEM_ARQUIVO = 24 * 1024 * 1024;
const SESSION_DAYS = 365;
const hex32 = /^[0-9a-f]{32}$/;

// ---- Utilidades ----

function answer(status, fields = null, extra = "") {
  let body = "";
  if (fields) for (const [key, value] of Object.entries(fields)) body += `${key}\t${clean(String(value), 100000)}\n`;
  return new Response(body + extra, { status, headers: { "Content-Type": "text/plain; charset=utf-8", "Cache-Control": "no-store" } });
}

function parse(text) {
  const out = {};
  for (const line of text.split("\n")) {
    const tab = line.indexOf("\t");
    if (tab > 0) out[line.slice(0, tab)] = line.slice(tab + 1).replace(/\r$/, "");
  }
  return out;
}

function clean(text, limit) {
  let out = String(text).replace(/[\t\r\n]/g, " ");
  const encoder = new TextEncoder();
  while (encoder.encode(out).length > limit) out = out.slice(0, -1);
  return out;
}

function hex(bytes) {
  return [...new Uint8Array(bytes)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function randomHex(bytes) {
  return hex(crypto.getRandomValues(new Uint8Array(bytes)));
}

function same(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

// HMAC-SHA256 with the Worker's secret: what the Ranga sends about its PC is hashed again here, so the database never
// holds anything that points to a PC.
async function hmac(secret, text) {
  const key = await crypto.subtle.importKey("raw", new TextEncoder().encode(secret), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  return hex(await crypto.subtle.sign("HMAC", key, new TextEncoder().encode(text)));
}

async function sha256(text) {
  return hex(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text)));
}

// "pbkdf2$iterações$sal$hash"
async function hashPassword(password, salt = randomHex(16), iterations = PBKDF2_ITERATIONS) {
  const key = await crypto.subtle.importKey("raw", new TextEncoder().encode(password), "PBKDF2", false, ["deriveBits"]);
  const bits = await crypto.subtle.deriveBits({ name: "PBKDF2", hash: "SHA-256", salt: new TextEncoder().encode(salt), iterations }, key, 256);
  return `pbkdf2$${iterations}$${salt}$${hex(bits)}`;
}

async function checkPassword(password, stored) {
  const [kind, iterations, salt] = stored.split("$");
  if (kind !== "pbkdf2") return false;
  return same(await hashPassword(password, salt, Number(iterations)), stored);
}

const now = () => Math.floor(Date.now() / 1000);

// Nome de conta: 3 a 20 letras, números, espaço, _ ou -, começando por letra ou número.
function validName(name) {
  return /^[\p{L}\p{N}][\p{L}\p{N} _-]{2,19}$/u.test(name);
}

function searchName(name) {
  return name.normalize("NFKC").toLowerCase();
}

// "Nome#1234"
function splitTag(text) {
  const at = text.lastIndexOf("#");
  if (at < 1) return null;
  const tag = Number(text.slice(at + 1));
  if (!Number.isInteger(tag) || tag < 1000 || tag > 9999) return null;
  return { name: text.slice(0, at).trim(), tag };
}

// No máximo `count` vezes por `seconds` para a chave (um IP, uma conta).
async function allowed(env, key, count, seconds) {
  const window = Math.floor(now() / seconds);
  const row = await env.DB.prepare("SELECT janela, contagem FROM limites WHERE chave = ?").bind(key).first();
  if (row && row.janela === window && row.contagem >= count) return false;
  if (row && row.janela === window) await env.DB.prepare("UPDATE limites SET contagem = contagem + 1 WHERE chave = ?").bind(key).run();
  else await env.DB.prepare("INSERT INTO limites (chave, janela, contagem) VALUES (?1, ?2, 1) ON CONFLICT(chave) DO UPDATE SET janela = ?2, contagem = 1").bind(key, window).run();
  return true;
}

// As linhas de `limites` de janelas que já passaram não servem mais; de vez em quando saem (cada chave usa a janela
// do seu tipo: entrar 900 s, msg 1 s, o resto 3600 s).
async function cleanLimits(env) {
  const t = now();
  await env.DB.prepare(
    "DELETE FROM limites WHERE (chave LIKE 'entrar:%' AND janela < ?1) OR (chave LIKE 'msg:%' AND janela < ?2) OR " +
      "(chave NOT LIKE 'entrar:%' AND chave NOT LIKE 'msg:%' AND janela < ?3)"
  ).bind(Math.floor(t / 900) - 1, t - 60, Math.floor(t / 3600) - 1).run();
}

async function newSession(env, account) {
  const key = randomHex(32);
  await env.DB.prepare("INSERT INTO sessoes (chave, conta, criada, usada) VALUES (?, ?, ?, ?)").bind(await sha256(key), account, now(), now()).run();
  return key;
}

async function sessionAccount(env, request) {
  const header = request.headers.get("Authorization") || "";
  if (!header.startsWith("Bearer ")) return null;
  const hash = await sha256(header.slice(7).trim());
  const row = await env.DB.prepare("SELECT s.conta AS conta, s.usada AS usada, c.nome AS nome, c.tag AS tag, c.banida AS banida FROM sessoes s JOIN contas c ON c.id = s.conta WHERE s.chave = ?").bind(hash).first();
  if (!row || row.banida || now() - row.usada > SESSION_DAYS * 86400) return null;
  // Marca o uso no máximo uma vez por hora, para não gastar escrita a cada pedido.
  if (now() - row.usada > 3600) await env.DB.prepare("UPDATE sessoes SET usada = ? WHERE chave = ?").bind(now(), hash).run();
  return { id: row.conta, name: row.nome, tag: row.tag, session: hash };
}

function pair(x, y) {
  return x < y ? [x, y] : [y, x];
}

async function friendship(env, x, y) {
  const [a, b] = pair(x, y);
  return env.DB.prepare("SELECT de, aceita FROM amizades WHERE a = ? AND b = ?").bind(a, b).first();
}

async function findByTag(env, text) {
  const parts = splitTag(text);
  if (!parts) return null;
  return env.DB.prepare("SELECT id, nome, tag, banida FROM contas WHERE nome_busca = ? AND tag = ?").bind(searchName(parts.name), parts.tag).first();
}

// ---- Prova de trabalho ----

async function challenge(env) {
  const stamp = now(), nonce = randomHex(8);
  const mac = (await hmac(env.PC_SAL, `desafio:${stamp}.${nonce}`)).slice(0, 32);
  return answer(200, { desafio: `${stamp}.${nonce}.${mac}`, bits: POW_BITS });
}

function zeroBits(hexText) {
  let bits = 0;
  for (const c of hexText) {
    const v = parseInt(c, 16);
    if (v === 0) { bits += 4; continue; }
    bits += Math.clz32(v) - 28;
    break;
  }
  return bits;
}

// O desafio é deste servidor, recente, não foi usado, e a resposta tem os bits zero.
async function proofOk(env, fields) {
  const text = fields.desafio || "", answerText = fields.resposta || "";
  const [stamp, nonce, mac] = text.split(".");
  if (!stamp || !nonce || !mac || answerText.length > 32) return false;
  if (!same(mac, (await hmac(env.PC_SAL, `desafio:${stamp}.${nonce}`)).slice(0, 32))) return false;
  if (now() - Number(stamp) > POW_SECONDS || Number(stamp) > now() + 60) return false;
  if (zeroBits(await sha256(`${text}:${answerText}`)) < POW_BITS) return false;
  await env.DB.prepare("DELETE FROM desafios WHERE quando < ?").bind(now() - POW_SECONDS - 60).run();
  const used = await env.DB.prepare("INSERT OR IGNORE INTO desafios (desafio, quando) VALUES (?, ?)").bind(text, now()).run();
  return used.meta.changes === 1;
}

async function blocked(env, by, who) {
  return !!(await env.DB.prepare("SELECT 1 FROM bloqueios WHERE quem = ? AND bloqueado = ?").bind(by, who).first());
}

// ---- Contas ----

async function create(env, ip, fields) {
  const name = (fields.nome || "").trim(), password = fields.senha || "";
  if (!validName(name)) return answer(400, { erro: "O nome precisa de 3 a 20 letras ou números (pode ter espaço, _ e -)." });
  if (password.length < 6 || password.length > 128) return answer(400, { erro: "A senha precisa de 6 a 128 caracteres." });
  if (!/^[0-9a-f]{64}$/.test(fields.pc || "") || !env.PC_SAL) return answer(400, { erro: "Atualize o Ranga para criar conta." });
  const pc = await hmac(env.PC_SAL, fields.pc);
  if (await env.DB.prepare("SELECT 1 FROM pcs_banidos WHERE pc = ?").bind(pc).first()) return answer(403, { erro: "Este PC não pode criar contas no Ranga." });
  if (!(await proofOk(env, fields))) return answer(400, { erro: "A verificação do Ranga não passou. Tente criar de novo." });
  const fromPc = await env.DB.prepare("SELECT COUNT(*) AS n FROM contas WHERE pc = ?").bind(pc).first();
  if (fromPc.n >= ACCOUNTS_PER_PC) return answer(429, { erro: `Este PC já criou ${ACCOUNTS_PER_PC} contas do Ranga, o máximo. Entre numa delas.` });
  if (!(await allowed(env, "criar:" + ip, 5, 3600))) return answer(429, { erro: "Muitas contas criadas daqui. Tente de novo daqui a uma hora." });
  const total = await env.DB.prepare("SELECT COUNT(*) AS n FROM contas").first();
  if (total.n >= MAX_ACCOUNTS) return answer(507, { erro: "O servidor do Ranga está cheio." });
  // Uma tag livre para o nome.
  const search = searchName(name);
  let tag = 0;
  for (let i = 0; i < 20 && tag === 0; i++) {
    const candidate = 1000 + Math.floor(Math.random() * 9000);
    const taken = await env.DB.prepare("SELECT 1 FROM contas WHERE nome_busca = ? AND tag = ?").bind(search, candidate).first();
    if (!taken) tag = candidate;
  }
  if (tag === 0) return answer(409, { erro: "Esse nome já tem gente demais. Escolha outro." });
  const id = randomHex(16);
  const recovery = randomHex(4).toUpperCase() + "-" + randomHex(4).toUpperCase() + "-" + randomHex(4).toUpperCase();
  await env.DB.prepare("INSERT INTO contas (id, nome, nome_busca, tag, senha, recuperacao, criada, pc) VALUES (?, ?, ?, ?, ?, ?, ?, ?)")
    .bind(id, name, search, tag, await hashPassword(password), await hashPassword(recovery), now(), pc).run();
  return answer(200, { conta: id, nome: name, tag, sessao: await newSession(env, id), recuperacao: recovery });
}

async function login(env, ip, fields) {
  if (!(await allowed(env, "entrar:" + ip, 20, 900))) return answer(429, { erro: "Muitas tentativas. Espere 15 minutos." });
  const account = await findByTag(env, fields.nome || "");
  const generic = { erro: "Nome#tag ou senha errados." };
  if (!account) return answer(403, generic);
  // Por conta e IP (10 em 15 min), e por conta no todo (100): quem erra a senha de um amigo de propósito não o deixa
  // de fora, a não ser com muitos IPs; e quem tenta adivinhar a senha de alguém continua devagar.
  if (!(await allowed(env, "entrar:" + account.id + ":" + ip, 10, 900)) || !(await allowed(env, "entrar:" + account.id, 100, 900)))
    return answer(429, { erro: "Muitas tentativas nesta conta. Espere 15 minutos." });
  const row = await env.DB.prepare("SELECT senha FROM contas WHERE id = ?").bind(account.id).first();
  if (!(await checkPassword(fields.senha || "", row.senha))) return answer(403, generic);
  if (account.banida) return answer(403, { erro: "Esta conta foi banida do Ranga." });
  return answer(200, { conta: account.id, nome: account.nome, tag: account.tag, sessao: await newSession(env, account.id) });
}

async function recover(env, ip, fields) {
  if (!(await allowed(env, "recuperar:" + ip, 10, 3600))) return answer(429, { erro: "Muitas tentativas. Tente daqui a uma hora." });
  const account = await findByTag(env, fields.nome || "");
  const generic = { erro: "Nome#tag ou código de recuperação errados." };
  if (!account) return answer(403, generic);
  if (account.banida) return answer(403, { erro: "Esta conta foi banida do Ranga." });
  const password = fields.senha || "";
  if (password.length < 6 || password.length > 128) return answer(400, { erro: "A senha nova precisa de 6 a 128 caracteres." });
  const row = await env.DB.prepare("SELECT recuperacao FROM contas WHERE id = ?").bind(account.id).first();
  if (!(await checkPassword((fields.recuperacao || "").trim().toUpperCase(), row.recuperacao))) return answer(403, generic);
  // Senha nova, código novo, e todas as sessões antigas fechadas.
  const recovery = randomHex(4).toUpperCase() + "-" + randomHex(4).toUpperCase() + "-" + randomHex(4).toUpperCase();
  await env.DB.batch([
    env.DB.prepare("UPDATE contas SET senha = ?, recuperacao = ? WHERE id = ?").bind(await hashPassword(password), await hashPassword(recovery), account.id),
    env.DB.prepare("DELETE FROM sessoes WHERE conta = ?").bind(account.id),
  ]);
  return answer(200, { conta: account.id, nome: account.nome, tag: account.tag, sessao: await newSession(env, account.id), recuperacao: recovery });
}

async function changePassword(env, me, fields) {
  const row = await env.DB.prepare("SELECT senha FROM contas WHERE id = ?").bind(me.id).first();
  if (!(await checkPassword(fields.senha || "", row.senha))) return answer(403, { erro: "A senha atual não confere." });
  const fresh = fields.nova || "";
  if (fresh.length < 6 || fresh.length > 128) return answer(400, { erro: "A senha nova precisa de 6 a 128 caracteres." });
  await env.DB.batch([
    env.DB.prepare("UPDATE contas SET senha = ? WHERE id = ?").bind(await hashPassword(fresh), me.id),
    env.DB.prepare("DELETE FROM sessoes WHERE conta = ? AND chave != ?").bind(me.id, me.session),
  ]);
  return answer(200, { ok: 1 });
}

// ---- Amigos ----

// Um cartão guardado antes da regra de storeCard sai também sem as linhas que o fechariam antes da hora.
function safeCard(card) {
  return card.replace(/\r/g, "").split("\n").filter((line, i) => line !== "fim_cartao" && !line.startsWith("midia\t") && !line.startsWith("visto\t") &&
    (i === 0 || line !== "ranga-perfil 1")).join("\n");
}

async function friends(env, me) {
  const { results } = await env.DB.prepare(
    "SELECT c.id AS id, c.nome AS nome, c.tag AS tag, c.cartao AS cartao, c.visto AS visto, f.aceita AS aceita, f.de AS de " +
      "FROM amizades f JOIN contas c ON c.id = CASE WHEN f.a = ?1 THEN f.b ELSE f.a END WHERE f.a = ?1 OR f.b = ?1"
  ).bind(me.id).all();
  const { results: mediaRows } = await env.DB.prepare(
    "SELECT m.conta AS conta, m.tipo AS tipo, m.versao AS versao FROM midia m JOIN amizades f ON (f.a = ?1 AND f.b = m.conta) OR (f.b = ?1 AND f.a = m.conta) WHERE f.aceita = 1"
  ).bind(me.id).all();
  let out = `eu\t${me.id}\t${me.name}#${me.tag}\n`;
  for (const row of results) {
    const who = `${row.id}\t${row.nome}#${row.tag}`;
    if (!row.aceita) {
      out += (row.de === me.id ? "pedido_enviado\t" : "pedido_recebido\t") + who + "\n";
      continue;
    }
    out += `amigo\t${who}\t${row.visto}\n`;
    {
      // Um amigo que ainda não mandou o cartão ganha um mínimo, para a foto e a capa irem junto.
      let card = row.cartao ? safeCard(row.cartao) : `ranga-perfil 1\nid\t${row.id}\nnome\t${row.nome}\nconta\t${row.nome}#${row.tag}\n`;
      if (!card.endsWith("\n")) card += "\n";
      for (const media of mediaRows.filter((m) => m.conta === row.id)) card += `midia\t${media.tipo}\t${media.versao}\n`;
      out += card + `visto\t${row.visto}\nfim_cartao\n`;
    }
  }
  const { results: blockedRows } = await env.DB.prepare("SELECT c.id AS id, c.nome AS nome, c.tag AS tag FROM bloqueios b JOIN contas c ON c.id = b.bloqueado WHERE b.quem = ?").bind(me.id).all();
  for (const row of blockedRows) out += `bloqueado\t${row.id}\t${row.nome}#${row.tag}\n`;
  return answer(200, null, out);
}

async function ask(env, me, fields) {
  if (!(await allowed(env, "pedir:" + me.id, 30, 3600))) return answer(429, { erro: "Muitos pedidos de amizade. Tente mais tarde." });
  const other = await findByTag(env, (fields.nome || "").trim());
  if (!other || other.banida) return answer(404, { erro: "Ninguém com esse Nome#tag." });
  if (other.id === me.id) return answer(400, { erro: "Esse é você." });
  if (await blocked(env, me.id, other.id)) return answer(400, { erro: "Você bloqueou essa pessoa. Desbloqueie na aba Amigos primeiro." });
  // Quem foi bloqueado não fica sabendo: para ele, a pessoa não existe.
  if (await blocked(env, other.id, me.id)) return answer(404, { erro: "Ninguém com esse Nome#tag." });
  const known = await friendship(env, me.id, other.id);
  const [a, b] = pair(me.id, other.id);
  if (known && known.aceita) return answer(200, { estado: "amigos" });
  if (known && known.de !== me.id) {
    await env.DB.prepare("UPDATE amizades SET aceita = 1 WHERE a = ? AND b = ?").bind(a, b).run();
    return answer(200, { estado: "amigos" });
  }
  if (known) return answer(200, { estado: "pedido" });
  await env.DB.prepare("INSERT INTO amizades (a, b, de, aceita, criada) VALUES (?, ?, ?, 0, ?)").bind(a, b, me.id, now()).run();
  return answer(200, { estado: "pedido" });
}

async function respond(env, me, fields) {
  const other = fields.conta || "";
  if (!hex32.test(other)) return answer(400, { erro: "Conta inválida." });
  const known = await friendship(env, me.id, other);
  if (!known || known.aceita || known.de === me.id) return answer(404, { erro: "Esse pedido não existe mais." });
  const [a, b] = pair(me.id, other);
  if (fields.aceitar === "1") await env.DB.prepare("UPDATE amizades SET aceita = 1 WHERE a = ? AND b = ?").bind(a, b).run();
  else await env.DB.prepare("DELETE FROM amizades WHERE a = ? AND b = ?").bind(a, b).run();
  return answer(200, { ok: 1 });
}

async function unfriend(env, me, fields) {
  const other = fields.conta || "";
  if (!hex32.test(other)) return answer(400, { erro: "Conta inválida." });
  const [a, b] = pair(me.id, other);
  await env.DB.prepare("DELETE FROM amizades WHERE a = ? AND b = ?").bind(a, b).run();
  return answer(200, { ok: 1 });
}

async function block(env, me, fields, on) {
  const other = fields.conta || "";
  if (!hex32.test(other) || other === me.id) return answer(400, { erro: "Conta inválida." });
  const [a, b] = pair(me.id, other);
  if (on) {
    await env.DB.batch([
      env.DB.prepare("INSERT OR IGNORE INTO bloqueios (quem, bloqueado, quando) VALUES (?, ?, ?)").bind(me.id, other, now()),
      env.DB.prepare("DELETE FROM amizades WHERE a = ? AND b = ?").bind(a, b),
    ]);
  } else {
    await env.DB.prepare("DELETE FROM bloqueios WHERE quem = ? AND bloqueado = ?").bind(me.id, other).run();
  }
  return answer(200, { ok: 1 });
}

// ---- Foto e capa ----

async function storeMedia(env, me, fields) {
  const kind = fields.tipo;
  if (kind !== "foto" && kind !== "capa") return answer(400, { erro: "Tipo inválido." });
  const data = (fields.dados || "").trim();
  if (data === "") {
    await env.DB.prepare("DELETE FROM midia WHERE conta = ? AND tipo = ?").bind(me.id, kind).run();
    return answer(200, { ok: 1 });
  }
  if (data.length > MAX_MEDIA || !/^[A-Za-z0-9+/=]+$/.test(data)) return answer(400, { erro: "Arquivo grande demais ou inválido." });
  if (!(await allowed(env, "midia:" + me.id, 20, 3600))) return answer(429, { erro: "Muitas trocas de foto. Tente mais tarde." });
  const version = (await sha256(data)).slice(0, 16);
  await env.DB.prepare("INSERT INTO midia (conta, tipo, versao, dados) VALUES (?1, ?2, ?3, ?4) ON CONFLICT(conta, tipo) DO UPDATE SET versao = ?3, dados = ?4")
    .bind(me.id, kind, version, data).run();
  return answer(200, { versao: version });
}

async function readMedia(env, me, account, kind) {
  if (!hex32.test(account || "") || (kind !== "foto" && kind !== "capa")) return answer(400);
  if (account !== me.id) {
    const known = await friendship(env, me.id, account);
    if (!known || !known.aceita) return answer(403);
  }
  const row = await env.DB.prepare("SELECT versao, dados FROM midia WHERE conta = ? AND tipo = ?").bind(account, kind).first();
  if (!row) return answer(404);
  return new Response(row.dados, { status: 200, headers: { "Content-Type": "text/plain", "Cache-Control": "no-store", "X-Versao": row.versao } });
}

// ---- O dono ----

async function ownerAccounts(env, search) {
  const like = "%" + searchName(search || "") + "%";
  const { results } = await env.DB.prepare(
    "SELECT id, nome, tag, criada, visto, banida, motivo FROM contas WHERE nome_busca LIKE ? ORDER BY criada DESC LIMIT 200"
  ).bind(like).all();
  return answer(200, null, results.map((c) => `c\t${c.id}\t${c.nome}#${c.tag}\t${c.criada}\t${c.visto}\t${c.banida}\t${clean(c.motivo || "", 200)}\n`).join(""));
}

async function ban(env, fields, on) {
  const id = fields.conta || "";
  if (!hex32.test(id)) return answer(400, { erro: "Conta inválida." });
  const row = await env.DB.prepare("SELECT pc FROM contas WHERE id = ?").bind(id).first();
  if (!row) return answer(404, { erro: "Essa conta não existe." });
  if (on) {
    const steps = [
      env.DB.prepare("UPDATE contas SET banida = 1, motivo = ? WHERE id = ?").bind(clean(fields.motivo || "", 200), id),
      env.DB.prepare("DELETE FROM sessoes WHERE conta = ?").bind(id),
      env.DB.prepare("DELETE FROM amizades WHERE a = ? OR b = ?").bind(id, id),
    ];
    if (row.pc) steps.push(env.DB.prepare("INSERT OR IGNORE INTO pcs_banidos (pc, quando) VALUES (?, ?)").bind(row.pc, now()));
    await env.DB.batch(steps);
  } else {
    const steps = [env.DB.prepare("UPDATE contas SET banida = 0, motivo = '' WHERE id = ?").bind(id)];
    if (row.pc) steps.push(env.DB.prepare("DELETE FROM pcs_banidos WHERE pc = ?").bind(row.pc));
    await env.DB.batch(steps);
  }
  return answer(200, { ok: 1 });
}

// ---- Perfil e conversas ----

async function storeCard(env, me, body) {
  if (!body.startsWith("ranga-perfil 1\n")) return answer(400, { erro: "Cartão inválido." });
  // O cartão fala pela conta: id e nome vêm dela, não do que o Ranga mandou. E fica dentro de si: na lista de
  // amigos ele vai entre "ranga-perfil 1" e "fim_cartao", seguido de "midia" e "visto" que o servidor põe; um
  // cartão com essas linhas fecharia antes e falaria pelo servidor (outro nome para um amigo, um pedido falso).
  const lines = body.replace(/\r/g, "").split("\n").filter((line, i) =>
    !line.startsWith("id\t") && !line.startsWith("nome\t") && !line.startsWith("conta\t") && !line.startsWith("midia\t") &&
    !line.startsWith("visto\t") && line !== "fim_cartao" && (i === 0 || line !== "ranga-perfil 1"));
  lines.splice(1, 0, `id\t${me.id}`, `nome\t${me.name}`, `conta\t${me.name}#${me.tag}`);
  await env.DB.prepare("UPDATE contas SET cartao = ?, visto = ? WHERE id = ?").bind(lines.join("\n"), now(), me.id).run();
  return answer(200, { ok: 1 });
}

async function send(env, me, fields) {
  const other = fields.para || "";
  const text = clean(fields.texto || "", MAX_TEXT);
  if (!hex32.test(other) || text.trim() === "") return answer(400, { erro: "Mensagem vazia." });
  const known = await friendship(env, me.id, other);
  if (!known || !known.aceita) return answer(403, { erro: "Só dá para conversar com amigos." });
  if (!(await allowed(env, "msg:" + me.id, 1, 1))) return answer(429, { erro: "Calma: uma mensagem por segundo." });
  await env.DB.prepare("INSERT INTO mensagens (de, para, quando, texto) VALUES (?, ?, ?, ?)").bind(me.id, other, now(), text).run();
  return answer(200, { ok: 1 });
}

async function messages(env, me, after, afterGroups) {
  const { results } = await env.DB.prepare(
    "SELECT numero, de, para, quando, texto FROM mensagens WHERE (para = ?1 OR de = ?1) AND numero > ?2 ORDER BY numero LIMIT 300"
  ).bind(me.id, after).all();
  let out = results.map((m) => `m\t${m.numero}\t${m.quando}\t${m.de}\t${m.para}\t${m.texto}\n`).join("");
  // As dos grupos desta conta (só um Ranga que pede desdeg as recebe).
  if (afterGroups !== null) {
    const { results: inGroups } = await env.DB.prepare(
      "SELECT numero, grupo, de, quando, texto FROM grupo_mensagens WHERE grupo IN (SELECT grupo FROM grupo_membros WHERE conta = ?1) AND numero > ?2 ORDER BY numero LIMIT 300"
    ).bind(me.id, afterGroups).all();
    out += inGroups.map((m) => `g\t${m.numero}\t${m.quando}\t${m.grupo}\t${m.de}\t${m.texto}\n`).join("");
  }
  // Quem está digitando para mim e até onde leu o que eu mandei (um Ranga antigo ignora essas linhas).
  const { results: states } = await env.DB.prepare("SELECT conta, digitando, lido FROM conversas WHERE com = ?").bind(me.id).all();
  for (const row of states) {
    if (row.digitando > now()) out += `d\t${row.conta}\n`;
    if (row.lido > 0) out += `l\t${row.conta}\t${row.lido}\n`;
  }
  return answer(200, null, out);
}

async function typing(env, me, fields) {
  const other = fields.para || "";
  if (!hex32.test(other)) return answer(400);
  // Só para um amigo: um estranho que saiba o id de alguém não faz aparecer "digitando" na tela dele.
  const known = await friendship(env, me.id, other);
  if (!known || !known.aceita) return answer(403);
  await env.DB.prepare("INSERT INTO conversas (conta, com, digitando) VALUES (?1, ?2, ?3) ON CONFLICT(conta, com) DO UPDATE SET digitando = ?3")
    .bind(me.id, other, now() + 6).run();
  return answer(200, { ok: 1 });
}

async function read(env, me, fields) {
  const other = fields.de || "";
  const number = Math.floor(Number(fields.numero || "0"));
  if (!hex32.test(other) || !Number.isFinite(number) || number <= 0) return answer(400);
  const known = await friendship(env, me.id, other);
  if (!known || !known.aceita) return answer(403);
  // Ler também encerra o "digitando" de quem leu e não respondeu.
  await env.DB.prepare(
    "INSERT INTO conversas (conta, com, lido) VALUES (?1, ?2, ?3) ON CONFLICT(conta, com) DO UPDATE SET lido = MAX(lido, ?3)"
  ).bind(me.id, other, number).run();
  return answer(200, { ok: 1 });
}

async function storeImage(env, me, fields) {
  // Para um amigo (para) ou para um grupo de que esta conta é membro (grupo).
  const group = fields.grupo || "";
  const other = group || fields.para || "";
  const data = (fields.dados || "").trim();
  if (!hex32.test(other) || data === "") return answer(400, { erro: "Imagem vazia." });
  if (data.length > MAX_IMAGE || !/^[A-Za-z0-9+/=]+$/.test(data)) return answer(400, { erro: "Imagem grande demais ou inválida." });
  if (group) {
    if (!(await member(env, group, me.id))) return answer(403, { erro: "Você não está nesse grupo." });
  } else {
    const known = await friendship(env, me.id, other);
    if (!known || !known.aceita) return answer(403, { erro: "Só dá para mandar para amigos." });
  }
  if (!(await allowed(env, "imagem:" + me.id, 30, 3600))) return answer(429, { erro: "Muitas imagens nesta hora. Espere um pouco." });
  if (!(await allowed(env, "imagemdia:" + me.id, IMAGES_PER_DAY, 86400))) return answer(429, { erro: `Limite de ${IMAGES_PER_DAY} imagens por dia. Amanhã libera.` });
  const used = await env.DB.prepare("SELECT COALESCE(SUM(LENGTH(dados)), 0) AS n FROM imagens WHERE de = ?").bind(me.id).first();
  if (used.n + data.length > IMAGE_BYTES) return answer(429, { erro: "Suas imagens dos últimos 30 dias encheram o espaço. As mais velhas saem sozinhas com o tempo." });
  const id = randomHex(16);
  await env.DB.prepare("INSERT INTO imagens (id, de, para, quando, dados) VALUES (?, ?, ?, ?, ?)").bind(id, me.id, other, now(), data).run();
  if (group) await env.DB.prepare("INSERT INTO grupo_mensagens (grupo, de, quando, texto) VALUES (?, ?, ?, ?)").bind(group, me.id, now(), `[[imagem|${id}]]`).run();
  else await env.DB.prepare("INSERT INTO mensagens (de, para, quando, texto) VALUES (?, ?, ?, ?)").bind(me.id, other, now(), `[[imagem|${id}]]`).run();
  return answer(200, { id });
}

async function readImage(env, me, id) {
  if (!hex32.test(id || "")) return answer(400);
  const row = await env.DB.prepare("SELECT de, para, dados FROM imagens WHERE id = ?").bind(id).first();
  if (!row) return answer(404);
  if (row.de !== me.id && row.para !== me.id && !(await member(env, row.para, me.id))) return answer(404);
  return new Response(row.dados, { status: 200, headers: { "Content-Type": "text/plain", "Cache-Control": "no-store" } });
}

// ---- Grupos ----

async function member(env, group, account) {
  return !!(await env.DB.prepare("SELECT 1 FROM grupo_membros WHERE grupo = ? AND conta = ?").bind(group, account).first());
}

async function groupOwner(env, group) {
  const row = await env.DB.prepare("SELECT dono FROM grupos WHERE id = ?").bind(group).first();
  return row ? row.dono : null;
}

async function groups(env, me) {
  const { results } = await env.DB.prepare(
    "SELECT g.id AS id, g.nome AS nome, g.dono AS dono FROM grupos g JOIN grupo_membros m ON m.grupo = g.id WHERE m.conta = ? ORDER BY g.criado"
  ).bind(me.id).all();
  let out = "";
  for (const row of results) out += `grupo\t${row.id}\t${row.nome}\t${row.dono}\n`;
  const { results: members } = await env.DB.prepare(
    "SELECT m.grupo AS grupo, c.id AS id, c.nome AS nome, c.tag AS tag FROM grupo_membros m JOIN contas c ON c.id = m.conta " +
      "WHERE m.grupo IN (SELECT grupo FROM grupo_membros WHERE conta = ?) ORDER BY m.entrou"
  ).bind(me.id).all();
  for (const row of members) out += `membro\t${row.grupo}\t${row.id}\t${row.nome}#${row.tag}\n`;
  return answer(200, null, out);
}

function groupName(text) {
  const name = clean(text || "", 60).trim();
  return name.length >= 1 ? name : null;
}

async function createGroup(env, me, fields) {
  const name = groupName(fields.nome);
  if (!name) return answer(400, { erro: "Dê um nome ao grupo." });
  if (!(await allowed(env, "grupo:" + me.id, 10, 3600))) return answer(429, { erro: "Muitos grupos criados. Tente mais tarde." });
  const mine = await env.DB.prepare("SELECT COUNT(*) AS n FROM grupo_membros WHERE conta = ?").bind(me.id).first();
  if (mine.n >= GROUPS_PER_ACCOUNT) return answer(400, { erro: `Você já está em ${GROUPS_PER_ACCOUNT} grupos, o máximo.` });
  const wanted = [...new Set((fields.membros || "").split(",").map((s) => s.trim()).filter((s) => hex32.test(s) && s !== me.id))].slice(0, GROUP_MAX - 1);
  const ok = [];
  for (const other of wanted) {
    const known = await friendship(env, me.id, other);
    if (known && known.aceita) ok.push(other);
  }
  const id = randomHex(16), t = now();
  const steps = [env.DB.prepare("INSERT INTO grupos (id, nome, dono, criado) VALUES (?, ?, ?, ?)").bind(id, name, me.id, t),
                 env.DB.prepare("INSERT INTO grupo_membros (grupo, conta, entrou) VALUES (?, ?, ?)").bind(id, me.id, t)];
  ok.forEach((other, i) => steps.push(env.DB.prepare("INSERT INTO grupo_membros (grupo, conta, entrou) VALUES (?, ?, ?)").bind(id, other, t + i + 1)));
  await env.DB.batch(steps);
  return answer(200, { id });
}

async function addToGroup(env, me, fields) {
  const group = fields.grupo || "", other = fields.conta || "";
  if (!hex32.test(group) || !hex32.test(other)) return answer(400, { erro: "Grupo ou conta inválidos." });
  if (!(await member(env, group, me.id))) return answer(403, { erro: "Você não está nesse grupo." });
  const known = await friendship(env, me.id, other);
  if (!known || !known.aceita) return answer(403, { erro: "Só dá para pôr um amigo seu no grupo." });
  const count = await env.DB.prepare("SELECT COUNT(*) AS n FROM grupo_membros WHERE grupo = ?").bind(group).first();
  if (count.n >= GROUP_MAX) return answer(400, { erro: `O grupo já tem ${GROUP_MAX} pessoas, o máximo.` });
  const theirs = await env.DB.prepare("SELECT COUNT(*) AS n FROM grupo_membros WHERE conta = ?").bind(other).first();
  if (theirs.n >= GROUPS_PER_ACCOUNT) return answer(400, { erro: "Esse amigo já está em grupos demais." });
  await env.DB.prepare("INSERT OR IGNORE INTO grupo_membros (grupo, conta, entrou) VALUES (?, ?, ?)").bind(group, other, now()).run();
  return answer(200, { ok: 1 });
}

// Sai do grupo (quem criou passa o grupo para o membro mais antigo); o grupo vazio some com as mensagens.
async function leaveGroup(env, group, account) {
  await env.DB.prepare("DELETE FROM grupo_membros WHERE grupo = ? AND conta = ?").bind(group, account).run();
  const next = await env.DB.prepare("SELECT conta FROM grupo_membros WHERE grupo = ? ORDER BY entrou LIMIT 1").bind(group).first();
  if (!next) {
    await env.DB.batch([
      env.DB.prepare("DELETE FROM grupos WHERE id = ?").bind(group),
      env.DB.prepare("DELETE FROM grupo_mensagens WHERE grupo = ?").bind(group),
      env.DB.prepare("DELETE FROM imagens WHERE para = ?").bind(group),
    ]);
  } else if ((await groupOwner(env, group)) === account) {
    await env.DB.prepare("UPDATE grupos SET dono = ? WHERE id = ?").bind(next.conta, group).run();
  }
}

async function removeFromGroup(env, me, fields) {
  const group = fields.grupo || "", other = fields.conta || "";
  if (!hex32.test(group) || !hex32.test(other)) return answer(400, { erro: "Grupo ou conta inválidos." });
  if ((await groupOwner(env, group)) !== me.id) return answer(403, { erro: "Só quem criou o grupo tira gente dele." });
  if (other === me.id) return answer(400, { erro: "Para sair, use Sair do grupo." });
  await leaveGroup(env, group, other);
  return answer(200, { ok: 1 });
}

async function renameGroup(env, me, fields) {
  const group = fields.grupo || "", name = groupName(fields.nome);
  if (!hex32.test(group) || !name) return answer(400, { erro: "Nome inválido." });
  if ((await groupOwner(env, group)) !== me.id) return answer(403, { erro: "Só quem criou o grupo muda o nome." });
  await env.DB.prepare("UPDATE grupos SET nome = ? WHERE id = ?").bind(name, group).run();
  return answer(200, { ok: 1 });
}

async function sendToGroup(env, me, fields) {
  const group = fields.grupo || "";
  const text = clean(fields.texto || "", MAX_TEXT);
  if (!hex32.test(group) || text.trim() === "") return answer(400, { erro: "Mensagem vazia." });
  if (!(await member(env, group, me.id))) return answer(403, { erro: "Você não está nesse grupo." });
  if (!(await allowed(env, "msg:" + me.id, 1, 1))) return answer(429, { erro: "Calma: uma mensagem por segundo." });
  await env.DB.prepare("INSERT INTO grupo_mensagens (grupo, de, quando, texto) VALUES (?, ?, ?, ?)").bind(group, me.id, now(), text).run();
  return answer(200, { ok: 1 });
}

// ---- Denúncias ----

async function report(env, me, fields) {
  const other = fields.conta || "";
  if (!hex32.test(other) || other === me.id) return answer(400, { erro: "Conta inválida." });
  if (!(await allowed(env, "denuncia:" + me.id, 10, 86400))) return answer(429, { erro: "Muitas denúncias hoje. Tente amanhã." });
  const reason = clean(fields.motivo || "", 300).trim();
  if (reason === "") return answer(400, { erro: "Diga o motivo da denúncia." });
  // As últimas 30 mensagens entre os dois vão junto, para o dono ver o que aconteceu.
  const { results } = await env.DB.prepare(
    "SELECT de, quando, texto FROM mensagens WHERE (de = ?1 AND para = ?2) OR (de = ?2 AND para = ?1) ORDER BY numero DESC LIMIT 30"
  ).bind(me.id, other).all();
  const excerpt = results.reverse().map((m) => `${m.quando} ${m.de === me.id ? "denunciante" : "denunciado"}: ${clean(m.texto, 500)}`).join("\n");
  await env.DB.prepare("INSERT INTO denuncias (de, contra, quando, motivo, trecho) VALUES (?, ?, ?, ?, ?)").bind(me.id, other, now(), reason, excerpt).run();
  return answer(200, { ok: 1 });
}

async function ownerReports(env) {
  const { results } = await env.DB.prepare(
    "SELECT d.id AS id, d.quando AS quando, d.motivo AS motivo, d.trecho AS trecho, d.contra AS contra, a.nome || '#' || a.tag AS quem, b.nome || '#' || b.tag AS alvo " +
      "FROM denuncias d LEFT JOIN contas a ON a.id = d.de LEFT JOIN contas b ON b.id = d.contra WHERE d.resolvida = 0 ORDER BY d.id DESC LIMIT 100"
  ).all();
  // O trecho vai numa linha só: as quebras viram " ¶ ".
  return answer(200, null, results.map((r) => `d\t${r.id}\t${r.quando}\t${r.quem || "?"}\t${r.contra}\t${r.alvo || "?"}\t${clean(r.motivo, 300)}\t${clean(r.trecho.replace(/\n/g, " ¶ "), 20000)}\n`).join(""));
}

// ---- Save na nuvem ----

const cloudPath = (text) => {
  const path = String(text || "");
  if (path.length < 3 || path.length > 400 || /[\t\r\n]/.test(path) || path.split(/[\\/]/).some((p) => p === ".." || p === "")) return null;
  if (!/^(saves|bios|emuladores)\\/i.test(path)) return null;
  return path;
};
const hex16 = /^[0-9a-f]{16}$/;

async function cloudList(env, me) {
  const { results } = await env.DB.prepare("SELECT caminho, hash, tamanho, bruto, partes, quando, pc FROM nuvem_arquivos WHERE conta = ?").bind(me.id).all();
  let used = 0, out = "";
  for (const r of results) {
    used += r.tamanho;
    out += `a\t${r.caminho}\t${r.hash}\t${r.tamanho}\t${r.bruto}\t${r.partes}\t${r.quando}\t${r.pc}\n`;
  }
  return answer(200, null, out + `cota\t${used}\t${NUVEM_MAX}\n`);
}

async function cloudPart(env, me, fields) {
  const hash = fields.hash || "", index = Number(fields.indice), data = (fields.dados || "").trim();
  if (!hex16.test(hash) || !Number.isInteger(index) || index < 0 || index > 64 || data === "") return answer(400, { erro: "Pedaço inválido." });
  if (data.length > NUVEM_PARTE || !/^[A-Za-z0-9+/=]+$/.test(data)) return answer(400, { erro: "Pedaço grande demais ou inválido." });
  if (!(await allowed(env, "nuvem:" + me.id, 2000, 3600))) return answer(429, { erro: "Envio demais para a nuvem nesta hora. Espere um pouco." });
  await env.DB.prepare("INSERT INTO nuvem_partes (conta, hash, indice, dados, quando) VALUES (?1, ?2, ?3, ?4, ?5) ON CONFLICT(conta, hash, indice) DO UPDATE SET dados = ?4, quando = ?5")
    .bind(me.id, hash, index, data, now()).run();
  return answer(200, { ok: 1 });
}

async function cloudClose(env, me, fields) {
  const path = cloudPath(fields.caminho), hash = fields.hash || "", before = fields.antes || "";
  const size = Number(fields.tamanho), raw = Number(fields.bruto), parts = Number(fields.partes);
  if (!path || !hex16.test(hash) || !Number.isInteger(size) || !Number.isInteger(raw) || !Number.isInteger(parts) || parts < 1 || parts > 64 || size < 0 || raw < 0)
    return answer(400, { erro: "Arquivo inválido." });
  if (size > NUVEM_ARQUIVO) return answer(400, { erro: "Esse save é grande demais para a nuvem." });
  const current = await env.DB.prepare("SELECT hash, tamanho FROM nuvem_arquivos WHERE conta = ? AND caminho = ?").bind(me.id, path).first();
  // Outro PC mandou outra versão depois da última que este PC viu: nada se perde sem o jogador escolher.
  if (current && current.hash !== hash && current.hash !== before) return answer(409, { erro: "conflito", hash: current.hash });
  const got = await env.DB.prepare("SELECT COUNT(*) AS n, COALESCE(SUM(LENGTH(dados)), 0) AS b FROM nuvem_partes WHERE conta = ? AND hash = ? AND indice < ?").bind(me.id, hash, parts).first();
  if (got.n !== parts) return answer(400, { erro: "Faltam pedaços desse save." });
  const used = await env.DB.prepare("SELECT COALESCE(SUM(tamanho), 0) AS n FROM nuvem_arquivos WHERE conta = ? AND caminho != ?").bind(me.id, path).first();
  if (used.n + size > NUVEM_MAX) return answer(507, { erro: `A nuvem desta conta encheu (${Math.round(NUVEM_MAX / 1048576)} MB).` });
  const steps = [
    env.DB.prepare("INSERT INTO nuvem_arquivos (conta, caminho, hash, tamanho, bruto, partes, quando, pc) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) " +
      "ON CONFLICT(conta, caminho) DO UPDATE SET hash = ?3, tamanho = ?4, bruto = ?5, partes = ?6, quando = ?7, pc = ?8")
      .bind(me.id, path, hash, size, raw, parts, now(), clean(fields.pc || "", 40)),
  ];
  // Os pedaços da versão de antes saem, se nenhum outro arquivo usa o mesmo conteúdo.
  if (current && current.hash !== hash) {
    steps.push(env.DB.prepare("DELETE FROM nuvem_partes WHERE conta = ?1 AND hash = ?2 AND NOT EXISTS (SELECT 1 FROM nuvem_arquivos WHERE conta = ?1 AND hash = ?2 AND caminho != ?3)")
      .bind(me.id, current.hash, path));
  }
  await env.DB.batch(steps);
  return answer(200, { ok: 1 });
}

async function cloudRead(env, me, hash, index) {
  if (!hex16.test(hash || "") || !Number.isInteger(index)) return answer(400);
  const row = await env.DB.prepare("SELECT dados FROM nuvem_partes WHERE conta = ? AND hash = ? AND indice = ?").bind(me.id, hash, index).first();
  if (!row) return answer(404);
  return new Response(row.dados, { status: 200, headers: { "Content-Type": "text/plain", "Cache-Control": "no-store" } });
}

async function cloudClear(env, me) {
  await env.DB.batch([
    env.DB.prepare("DELETE FROM nuvem_arquivos WHERE conta = ?").bind(me.id),
    env.DB.prepare("DELETE FROM nuvem_partes WHERE conta = ?").bind(me.id),
  ]);
  return answer(200, { ok: 1 });
}

// Pedaços de envios que nunca fecharam (o PC caiu no meio), depois de um dia.
async function cleanCloud(env) {
  await env.DB.prepare(
    "DELETE FROM nuvem_partes WHERE quando < ? AND NOT EXISTS (SELECT 1 FROM nuvem_arquivos a WHERE a.conta = nuvem_partes.conta AND a.hash = nuvem_partes.hash)"
  ).bind(now() - 86400).run();
}

async function cleanImages(env) {
  await env.DB.prepare("DELETE FROM imagens WHERE quando < ?").bind(now() - IMAGE_DAYS * 86400).run();
}

// ---- Entrada ----

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const path = url.pathname;
    const ip = request.headers.get("CF-Connecting-IP") || "?";
    if (Math.random() < 0.01) ctx.waitUntil(cleanLimits(env).catch(() => {}));
    if (Math.random() < 0.002) ctx.waitUntil(cleanImages(env).catch(() => {}));
    if (Math.random() < 0.002) ctx.waitUntil(cleanCloud(env).catch(() => {}));
    let fields = {}, body = "";
    if (request.method === "POST") {
      const limit = path === "/midia" ? MAX_MEDIA + 1024 : path === "/imagem" ? MAX_IMAGE + 1024 : path === "/nuvem/parte" ? NUVEM_PARTE + 1024 : MAX_BODY;
      if (Number(request.headers.get("Content-Length") || "0") > limit) return answer(400, { erro: "Pedido grande demais." });
      body = await request.text();
      if (body.length > limit) return answer(400, { erro: "Pedido grande demais." });
      fields = parse(body);
    } else if (request.method !== "GET") {
      return answer(404);
    }
    if (request.method === "GET" && path === "/conta/desafio") return challenge(env);
    if (request.method === "POST" && path === "/conta/criar") return create(env, ip, fields);
    if (path.startsWith("/dono/")) {
      // Uma chave errada conta como tentativa; passou de 20 na hora, esse IP espera (mesmo com a chave certa).
      const tries = await env.DB.prepare("SELECT janela, contagem FROM limites WHERE chave = ?").bind("dono:" + ip).first();
      if (tries && tries.janela === Math.floor(now() / 3600) && tries.contagem > 20) return answer(429, { erro: "Sem permissão." });
      if (!env.DONO_CHAVE || !same(request.headers.get("X-Ranga-Dono") || "", env.DONO_CHAVE)) {
        await allowed(env, "dono:" + ip, 1000000, 3600);
        return answer(403, { erro: "Sem permissão." });
      }
      if (request.method === "GET" && path === "/dono/contas") return ownerAccounts(env, url.searchParams.get("busca"));
      if (request.method === "POST" && path === "/dono/banir") return ban(env, fields, true);
      if (request.method === "POST" && path === "/dono/desbanir") return ban(env, fields, false);
      if (request.method === "GET" && path === "/dono/denuncias") return ownerReports(env);
      if (request.method === "POST" && path === "/dono/denuncia") {
        await env.DB.prepare("UPDATE denuncias SET resolvida = 1 WHERE id = ?").bind(Number(fields.id) || 0).run();
        return answer(200, { ok: 1 });
      }
      if (request.method === "POST" && path === "/dono/salas") {
        // Só endereços simples: o Ranga confere de novo antes de usar.
        const host = /^[A-Za-z0-9][A-Za-z0-9.-]{0,119}$/, way = /^[A-Za-z0-9][A-Za-z0-9.-]{0,110}:[0-9]{1,5}$/;
        let text = `servidor\t${host.test(fields.servidor || "") ? fields.servidor : ""}\n`;
        for (const key of ["switch", "3ds", "classicos", "xbox", "ps3"]) if (way.test(fields[key] || "")) text += `${key}\t${fields[key]}\n`;
        // O código do grupo vai junto, só para quem tem conta (GET /salas): o Ranga dos amigos o leva às mesas.
        if (/^[A-Za-z0-9_-]{8,64}$/.test(fields.grupo || "")) text += `grupo\t${fields.grupo}\n`;
        await env.DB.prepare("INSERT INTO ajustes (chave, valor) VALUES ('salas', ?) ON CONFLICT (chave) DO UPDATE SET valor = excluded.valor").bind(text).run();
        return answer(200, { ok: 1 });
      }
      return answer(404);
    }
    if (request.method === "POST" && path === "/conta/entrar") return login(env, ip, fields);
    if (request.method === "POST" && path === "/conta/recuperar") return recover(env, ip, fields);
    const me = await sessionAccount(env, request);
    if (!me) return answer(401, { erro: "Entre na sua conta de novo." });
    if (request.method === "POST") {
      if (path === "/conta/sair") {
        await env.DB.prepare("DELETE FROM sessoes WHERE chave = ?").bind(me.session).run();
        return answer(200, { ok: 1 });
      }
      if (path === "/conta/senha") return changePassword(env, me, fields);
      if (path === "/perfil") return storeCard(env, me, body);
      if (path === "/amigos/pedir") return ask(env, me, fields);
      if (path === "/amigos/responder") return respond(env, me, fields);
      if (path === "/amigos/remover") return unfriend(env, me, fields);
      if (path === "/mensagem") return send(env, me, fields);
      if (path === "/midia") return storeMedia(env, me, fields);
      if (path === "/digitando") return typing(env, me, fields);
      if (path === "/lido") return read(env, me, fields);
      if (path === "/imagem") return storeImage(env, me, fields);
      if (path === "/bloquear") return block(env, me, fields, true);
      if (path === "/desbloquear") return block(env, me, fields, false);
      if (path === "/denunciar") return report(env, me, fields);
      if (path === "/grupo/criar") return createGroup(env, me, fields);
      if (path === "/grupo/adicionar") return addToGroup(env, me, fields);
      if (path === "/grupo/remover") return removeFromGroup(env, me, fields);
      if (path === "/grupo/renomear") return renameGroup(env, me, fields);
      if (path === "/grupo/mensagem") return sendToGroup(env, me, fields);
      if (path === "/grupo/sair") {
        if (!hex32.test(fields.grupo || "") || !(await member(env, fields.grupo, me.id))) return answer(404, { erro: "Você não está nesse grupo." });
        await leaveGroup(env, fields.grupo, me.id);
        return answer(200, { ok: 1 });
      }
      if (path === "/nuvem/parte") return cloudPart(env, me, fields);
      if (path === "/nuvem/fechar") return cloudClose(env, me, fields);
      if (path === "/nuvem/limpar") return cloudClear(env, me);
    } else {
      if (path === "/amigos") return friends(env, me);
      if (path === "/midia") return readMedia(env, me, url.searchParams.get("conta"), url.searchParams.get("tipo"));
      if (path === "/imagem") return readImage(env, me, url.searchParams.get("id"));
      if (path === "/mensagens") {
        const groupsAfter = url.searchParams.get("desdeg");
        return messages(env, me, Math.max(0, Number(url.searchParams.get("desde") || "0") || 0), groupsAfter === null ? null : Math.max(0, Number(groupsAfter) || 0));
      }
      if (path === "/grupos") return groups(env, me);
      if (path === "/salas") {
        const row = await env.DB.prepare("SELECT valor FROM ajustes WHERE chave = 'salas'").first();
        return answer(200, null, row ? row.valor : "servidor\t\n");
      }
      if (path === "/nuvem") return cloudList(env, me);
      if (path === "/nuvem/parte") return cloudRead(env, me, url.searchParams.get("hash"), Number(url.searchParams.get("indice")));
    }
    return answer(404);
  },
};
