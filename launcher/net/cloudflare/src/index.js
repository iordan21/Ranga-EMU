// A central do Ranga na Cloudflare (Workers + D1, plano grátis): perfis, amigos e chat, sempre no ar.
// Fala o mesmo HTTP que a sala dos clássicos (launcher/net/RangaSala.cpp), então o Ranga a usa como "central" e cai
// para a sala do PC quando ela não responde (PlayerProfile.cpp SocialRequest).
//
//   POST /ranga/perfil   "segredo<TAB>hex" e o cartão; só o Ranga que criou um perfil o troca depois
//   GET  /ranga/perfis   todos os cartões, cada um com "visto"
//   POST /ranga/chat     linhas "segredo", "id" e "texto"; uma mensagem por segundo por perfil
//   GET  /ranga/chat?desde=N
//
// O código do grupo (segredo GRUPO do Worker) vem no cabeçalho X-Ranga-Grupo; sem ele, nada é lido nem gravado.

const MAX_PROFILES = 200;
const MAX_BODY = 512 * 1024;
const KEPT_MESSAGES = 500;
const MAX_TEXT = 500;

const hex32 = /^[0-9a-fA-F]{32}$/;

function answer(status, body = "") {
  return new Response(body, { status, headers: { "Content-Type": "application/octet-stream", "Cache-Control": "no-store" } });
}

function lineValue(text, key) {
  for (const line of text.split("\n")) {
    if (line.startsWith(key + "\t")) return line.slice(key.length + 1).replace(/\r$/, "");
  }
  return "";
}

// Sem tabulação nem quebra de linha, e no máximo limit bytes de UTF-8 sem cortar uma letra no meio.
function clean(text, limit) {
  let out = text.replace(/[\t\r\n]/g, " ");
  const encoder = new TextEncoder();
  while (encoder.encode(out).length > limit) out = out.slice(0, -1);
  return out;
}

// Comparação que não entrega pelo tempo onde o segredo difere.
function same(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

async function storeProfile(env, body) {
  const first = body.indexOf("\n");
  if (!body.startsWith("segredo\t") || first < 0) return 400;
  const secret = body.slice(8, first).trim();
  const card = body.slice(first + 1);
  const id = lineValue(card, "id");
  if (!card.startsWith("ranga-perfil 1\n") || !hex32.test(id) || !hex32.test(secret)) return 400;
  const known = await env.DB.prepare("SELECT segredo FROM perfis WHERE id = ?").bind(id).first();
  if (known && !same(known.segredo, secret)) return 403;
  if (!known) {
    const count = await env.DB.prepare("SELECT COUNT(*) AS n FROM perfis").first();
    if (count.n >= MAX_PROFILES) return 507;
  }
  const now = Math.floor(Date.now() / 1000);
  await env.DB.prepare(
    "INSERT INTO perfis (id, segredo, nome, cartao, visto) VALUES (?1, ?2, ?3, ?4, ?5) " +
      "ON CONFLICT(id) DO UPDATE SET nome = ?3, cartao = ?4, visto = ?5"
  ).bind(id, secret, clean(lineValue(card, "nome"), 40), card, now).run();
  return 200;
}

async function allProfiles(env) {
  const { results } = await env.DB.prepare("SELECT cartao, visto FROM perfis").all();
  let out = "";
  for (const row of results) {
    let card = row.cartao;
    if (!card.endsWith("\n")) card += "\n";
    out += card + "visto\t" + row.visto + "\n";
  }
  return out;
}

async function say(env, body) {
  const secret = lineValue(body, "segredo"), id = lineValue(body, "id");
  const text = clean(lineValue(body, "texto"), MAX_TEXT);
  if (!hex32.test(id)) return 400;
  const profile = await env.DB.prepare("SELECT segredo, nome FROM perfis WHERE id = ?").bind(id).first();
  if (!profile || !same(profile.segredo, secret)) return 403;
  if (text.trim() === "") return 400;
  const now = Math.floor(Date.now() / 1000);
  const last = await env.DB.prepare("SELECT MAX(quando) AS q FROM chat WHERE id = ?").bind(id).first();
  if (last && last.q !== null && now - last.q < 1) return 429;
  await env.DB.batch([
    env.DB.prepare("INSERT INTO chat (quando, id, nome, texto) VALUES (?, ?, ?, ?)").bind(now, id, clean(profile.nome || "", 40), text),
    env.DB.prepare("DELETE FROM chat WHERE numero <= (SELECT MAX(numero) FROM chat) - ?").bind(KEPT_MESSAGES),
  ]);
  return 200;
}

async function chatSince(env, after) {
  const { results } = await env.DB.prepare("SELECT numero, quando, id, nome, texto FROM chat WHERE numero > ? ORDER BY numero").bind(after).all();
  return results.map((m) => `m\t${m.numero}\t${m.quando}\t${m.id}\t${m.nome}\t${m.texto}\n`).join("");
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname;
    if (path !== "/ranga/perfil" && path !== "/ranga/perfis" && path !== "/ranga/chat") return answer(404);
    if (!env.GRUPO || !same(request.headers.get("X-Ranga-Grupo") || "", env.GRUPO)) return answer(401);
    if (request.method === "POST") {
      const length = Number(request.headers.get("Content-Length") || "0");
      if (length > MAX_BODY) return answer(400);
      const body = await request.text();
      if (body.length === 0 || body.length > MAX_BODY) return answer(400);
      if (path === "/ranga/perfil") return answer(await storeProfile(env, body));
      if (path === "/ranga/chat") return answer(await say(env, body));
      return answer(404);
    }
    if (request.method !== "GET") return answer(404);
    if (path === "/ranga/perfis") return answer(200, await allProfiles(env));
    if (path === "/ranga/chat") return answer(200, await chatSince(env, Math.max(0, Number(url.searchParams.get("desde") || "0") || 0)));
    return answer(404);
  },
};
