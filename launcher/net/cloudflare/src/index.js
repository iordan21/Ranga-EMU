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
//
// O dono do Ranga (cabeçalho X-Ranga-Dono com o segredo DONO_CHAVE):
//   GET  /dono/contas?busca=texto                         as contas (as mais novas primeiro)
//   POST /dono/banir       conta, motivo                  (fecha as sessões, desfaz as amizades, o PC dela não cria outra)
//   POST /dono/desbanir    conta

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
  if (!(await allowed(env, "entrar:" + account.id, 10, 900))) return answer(429, { erro: "Muitas tentativas nesta conta. Espere 15 minutos." });
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
      let card = row.cartao || `ranga-perfil 1\nid\t${row.id}\nnome\t${row.nome}\nconta\t${row.nome}#${row.tag}\n`;
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
  // O cartão fala pela conta: id e nome vêm dela, não do que o Ranga mandou.
  const lines = body.split("\n").filter((line) => !line.startsWith("id\t") && !line.startsWith("nome\t") && !line.startsWith("conta\t"));
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

async function messages(env, me, after) {
  const { results } = await env.DB.prepare(
    "SELECT numero, de, para, quando, texto FROM mensagens WHERE (para = ?1 OR de = ?1) AND numero > ?2 ORDER BY numero LIMIT 300"
  ).bind(me.id, after).all();
  let out = results.map((m) => `m\t${m.numero}\t${m.quando}\t${m.de}\t${m.para}\t${m.texto}\n`).join("");
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
  await env.DB.prepare("INSERT INTO conversas (conta, com, digitando) VALUES (?1, ?2, ?3) ON CONFLICT(conta, com) DO UPDATE SET digitando = ?3")
    .bind(me.id, other, now() + 6).run();
  return answer(200, { ok: 1 });
}

async function read(env, me, fields) {
  const other = fields.de || "";
  const number = Math.floor(Number(fields.numero || "0"));
  if (!hex32.test(other) || !Number.isFinite(number) || number <= 0) return answer(400);
  // Ler também encerra o "digitando" de quem leu e não respondeu.
  await env.DB.prepare(
    "INSERT INTO conversas (conta, com, lido) VALUES (?1, ?2, ?3) ON CONFLICT(conta, com) DO UPDATE SET lido = MAX(lido, ?3)"
  ).bind(me.id, other, number).run();
  return answer(200, { ok: 1 });
}

async function storeImage(env, me, fields) {
  const other = fields.para || "";
  const data = (fields.dados || "").trim();
  if (!hex32.test(other) || data === "") return answer(400, { erro: "Imagem vazia." });
  if (data.length > MAX_IMAGE || !/^[A-Za-z0-9+/=]+$/.test(data)) return answer(400, { erro: "Imagem grande demais ou inválida." });
  const known = await friendship(env, me.id, other);
  if (!known || !known.aceita) return answer(403, { erro: "Só dá para mandar para amigos." });
  if (!(await allowed(env, "imagem:" + me.id, 30, 3600))) return answer(429, { erro: "Muitas imagens nesta hora. Espere um pouco." });
  const id = randomHex(16);
  await env.DB.prepare("INSERT INTO imagens (id, de, para, quando, dados) VALUES (?, ?, ?, ?, ?)").bind(id, me.id, other, now(), data).run();
  await env.DB.prepare("INSERT INTO mensagens (de, para, quando, texto) VALUES (?, ?, ?, ?)").bind(me.id, other, now(), `[[imagem|${id}]]`).run();
  return answer(200, { id });
}

async function readImage(env, me, id) {
  if (!hex32.test(id || "")) return answer(400);
  const row = await env.DB.prepare("SELECT de, para, dados FROM imagens WHERE id = ?").bind(id).first();
  if (!row || (row.de !== me.id && row.para !== me.id)) return answer(404);
  return new Response(row.dados, { status: 200, headers: { "Content-Type": "text/plain", "Cache-Control": "no-store" } });
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
    let fields = {}, body = "";
    if (request.method === "POST") {
      const limit = path === "/midia" ? MAX_MEDIA + 1024 : path === "/imagem" ? MAX_IMAGE + 1024 : MAX_BODY;
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
    } else {
      if (path === "/amigos") return friends(env, me);
      if (path === "/midia") return readMedia(env, me, url.searchParams.get("conta"), url.searchParams.get("tipo"));
      if (path === "/imagem") return readImage(env, me, url.searchParams.get("id"));
      if (path === "/mensagens") return messages(env, me, Math.max(0, Number(url.searchParams.get("desde") || "0") || 0));
    }
    return answer(404);
  },
};
