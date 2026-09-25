import { basename } from "node:path";
import type { AgentState, AgentStatus, HookBase, HookPreTool, HookStopFailure } from "./types.ts";
import { publishAgentState, clearAgentState } from "./mqtt.ts";
import { hasPaneFor } from "./actions.ts";

/** Cuanto se mantiene el retained de un agente muerto antes de borrarlo. */
const OFFLINE_TTL_MS = 10 * 60 * 1000;

const agents = new Map<string, AgentState>();
const reapers = new Map<string, ReturnType<typeof setTimeout>>();

/**
 * cwd por agente. Se queda AQUI, no viaja en el payload MQTT: la placa no lo
 * necesita y son bytes en un mensaje que vigilamos. Lo usa la fase 4 para
 * localizar el panel de tmux o lanzar claude -p en el sitio correcto.
 */
const cwds = new Map<string, string>();

export function cwdOf(agentId: string): string | undefined {
  return cwds.get(agentId);
}

export function knownCwds(): Record<string, string> {
  return Object.fromEntries(cwds);
}

const now = () => Math.floor(Date.now() / 1000);

/**
 * Una fila = una SESION. Los subagentes no son filas aparte: se agrupan bajo
 * su sesion. Antes cada subagente aparecia suelto y con la misma etiqueta que
 * su padre (las dos salen del cwd), asi que en pantalla veias el mismo nombre
 * dos veces, uno en reposo y otro trabajando, sin forma de saber que eran lo
 * mismo.
 */
function agentIdOf(h: HookBase): string {
  return h.session_id.replace(/-/g, "").slice(0, 8);
}

function subIdOf(h: HookBase): string | null {
  return h.agent_id ? h.agent_id.replace(/-/g, "").slice(0, 8) : null;
}

interface Sub {
  type: string;
  status: AgentStatus;
  tool: string | null;
  detail: string | null;
  raw: string | null;
  rx: number;              // ms, para caducar subagentes que mueren sin avisar
}

/** sessionId -> subagentes vivos (los que han usado alguna herramienta) */
const subs = new Map<string, Map<string, Sub>>();

/**
 * sessionId -> subagentes arrancados y aun no terminados.
 *
 * Hace falta aparte de `subs` porque de un subagente solo nos enteramos
 * cuando usa una herramienta: mientras piensa no manda nada, y la sesion
 * padre, que ya cerro su turno, aparecia "en reposo" mintiendo. Esto es lo
 * mismo que Claude Code enseña en el terminal como "waiting for N background
 * agents to finish".
 */
const pending = new Map<string, Set<string>>();

export function onSubagentStart(h: HookBase): AgentState | null {
  const id = agentIdOf(h), sub = subIdOf(h);
  if (!sub) return null;
  if (!pending.has(id)) pending.set(id, new Set());
  pending.get(id)!.add(sub);
  return own.has(id) ? republish(id) : null;
}

/** Un subagente sin noticias tanto rato se da por muerto. */
const SUB_TTL_MS = 3 * 60 * 1000;

/** Orden de interes; el menor manda. Mismo criterio que usa la placa. */
const RANK: Record<AgentStatus, number> = {
  waiting_permission: 0, error: 1, working: 2, thinking: 3, idle: 4, offline: 5,
};

function liveSubs(sid: string): Sub[] {
  const m = subs.get(sid);
  if (!m) return [];
  const cutoff = Date.now() - SUB_TTL_MS;
  for (const [k, v] of m) if (v.rx < cutoff) m.delete(k);
  return [...m.values()];
}

/** Directorios contenedores que no aportan nada como prefijo de la etiqueta. */
const NOISE = new Set([
  "projects", "repos", "repo", "src", "code", "dev", "work", "git",
  "documents", "desktop", "users", "home", "workspace", "sites",
  "tmp", "private", "var", "opt",
]);

/**
 * "brain/api" se lee mejor que "api", pero "Projects/sherpa" no aporta nada.
 * Se anade el directorio padre solo si no es un contenedor generico y si los
 * dos segmentos caben en el ancho de la fila (24 chars).
 */
/**
 * De donde sale el nombre de una fila, por orden de preferencia:
 *
 *   1. El que le has puesto tu con /rename o --name. Claude Code lo guarda en
 *      <transcript sin .jsonl>/custom-title.json. Gana siempre: si te has
 *      molestado en llamarla "brain", eso es lo que quieres leer.
 *   2. El aiTitle del transcript, que Claude Code genera solo con el tema.
 *   3. El nombre de la carpeta, que no distingue dos sesiones en el mismo repo.
 *
 * El fichero del nombre propio es diminuto, asi que se relee cada 30 s. El
 * transcript puede pesar megas: ese solo cada 5 minutos, y solo si no hay
 * nombre propio.
 */
const titles = new Map<string, string>();
const titleRead = new Map<string, number>();
const aiRead = new Map<string, number>();
const TITLE_REFRESH_MS = 30 * 1000;
const AI_REFRESH_MS = 5 * 60 * 1000;

async function readCustomTitle(transcript: string): Promise<string | null> {
  try {
    const dir = transcript.replace(/\.jsonl$/, "");
    const raw = await Bun.file(`${dir}/custom-title.json`).text();
    const t = (JSON.parse(raw) as { customTitle?: string }).customTitle;
    return t && t.trim() ? t.trim() : null;
  } catch {
    return null;                 // la sesion no tiene nombre propio: normal
  }
}

async function readTitle(path: string): Promise<string | null> {
  try {
    const txt = await Bun.file(path).text();
    let found: string | null = null;
    // La ultima aparicion gana: el titulo se reescribe si el tema cambia.
    for (const line of txt.split("\n")) {
      if (!line.includes('"aiTitle"')) continue;
      try {
        const t = (JSON.parse(line) as { aiTitle?: string }).aiTitle;
        if (t) found = t;
      } catch { /* linea a medio escribir */ }
    }
    return found;
  } catch {
    return null;
  }
}

/**
 * Lanzado y olvidado a proposito: un hook de telemetria no puede esperar a
 * leer un fichero de megas. El primer evento se queda con el nombre de la
 * carpeta y el siguiente ya trae el titulo.
 */
function rememberTitle(id: string, path?: string) {
  if (!path) return;
  const ahora = Date.now();
  if (ahora - (titleRead.get(id) ?? 0) < TITLE_REFRESH_MS) return;
  titleRead.set(id, ahora);

  void (async () => {
    let t = await readCustomTitle(path);

    if (!t && ahora - (aiRead.get(id) ?? 0) >= AI_REFRESH_MS) {
      aiRead.set(id, ahora);
      t = await readTitle(path);
    }
    if (!t) return;

    if (titles.get(id) !== t) {
      titles.set(id, t);
      if (own.has(id)) republish(id);      // repinta con el nombre bueno
    }
  })();
}

/** Recorta a lo que cabe en una fila, ya en ASCII. */
function shortLabel(raw: string): string {
  const l = toAscii(raw);
  // 30 es lo que entra en la fila con Montserrat 16 (277 px de ancho util).
  return l.length > 30 ? l.slice(0, 29) + "..." : l;
}

function labelOf(h: HookBase, id?: string): string {
  let dir = "?";
  if (h.cwd) {
    const parts = h.cwd.split("/").filter(Boolean);
    const last = parts.at(-1) ?? "?";
    const parent = parts.at(-2);
    const two = parent ? `${parent}/${last}` : last;
    dir = parent && !NOISE.has(parent.toLowerCase()) && two.length <= 24 ? two : last;
  }
  // El agent_type ya no va en la etiqueta: los subagentes se agrupan bajo su
  // sesion y su tipo viaja aparte en subType.
  return shortLabel(titles.get(id ?? "") ?? dir);
}

/**
 * Las fuentes Montserrat que trae LVGL cubren ASCII y sus iconos, y nada mas:
 * ni tildes, ni ñ, ni ¿, ni el punto medio. Verificado leyendo el cmap del
 * .c de la fuente. Un titulo como "Extension Chrome" se ve entero; con la
 * tilde original se veria un hueco.
 *
 * Se translitera aqui y no en la placa: el ESP32 no tiene por que saber de
 * esto, y ademas asi el payload MQTT se queda en ASCII de una vez.
 */
const ACENTOS: Record<string, string> = {
  á:"a", é:"e", í:"i", ó:"o", ú:"u", ü:"u", ñ:"n", ç:"c",
  Á:"A", É:"E", Í:"I", Ó:"O", Ú:"U", Ü:"U", Ñ:"N", Ç:"C",
  à:"a", è:"e", ì:"i", ò:"o", ù:"u", â:"a", ê:"e", î:"i", ô:"o", û:"u",
  "¿":"", "¡":"", "·":"-", "—":"-", "–":"-", "“":'"', "”":'"', "‘":"'", "’":"'",
  "\u2026":"...",
};

function toAscii(s: string): string {
  let out = "";
  for (const ch of s) {
    const rep = ACENTOS[ch];
    if (rep !== undefined) out += rep;
    else if (ch.charCodeAt(0) < 128) out += ch;
    else out += "?";
  }
  return out;
}

function truncate(s: string, n = 64): string {
  const flat = toAscii(s).replace(/\s+/g, " ").trim();
  return flat.length > n ? flat.slice(0, n - 1) + "..." : flat;
}

/**
 * Que esta haciendo, en cristiano.
 *
 * La fila se lee de reojo desde el otro lado de la mesa. Un
 * "sed -n '30,70p' lib/delivery/types.ts" no dice nada en esa situacion;
 * "Leyendo types.ts" si. El comando literal no se pierde: viaja aparte en
 * `raw` y se ve entero al tocar la fila.
 */
const BASH: Array<[RegExp, string]> = [
  [/\bgit\s+commit/,                          "Haciendo commit"],
  [/\bgit\s+push/,                            "Subiendo cambios a git"],
  [/\bgit\s+(diff|status|log|show)/,          "Revisando cambios en git"],
  [/\bgit\s+(checkout|switch|branch|merge|rebase)/, "Moviendose de rama"],
  [/\bgit\s+(add|restore|stash)/,             "Preparando cambios"],
  [/\bgit\b/,                                 "Trasteando con git"],
  [/\b(pytest|jest|vitest|go\s+test|cargo\s+test)\b|\b(npm|bun|yarn|pnpm)\s+(run\s+)?test/, "Lanzando los tests"],
  [/\b(npm|bun|yarn|pnpm)\s+(run\s+)?(build|compile)\b|\bmake\b|\bcargo\s+build\b|\bgradle\b/, "Compilando"],
  [/\bpio\s+run[^|]*-t\s+upload/,            "Flasheando la placa"],
  [/\bpio\s+run/,                             "Compilando el firmware"],
  [/\b(npm|bun|yarn|pnpm|pip3?|brew|cargo)\s+(install|add|i)\b/, "Instalando dependencias"],
  [/\b(rg|grep|ag|ack)\b/,                    "Buscando en el codigo"],
  [/\bfind\b|\bls\b/,                       "Mirando que hay"],
  [/\b(cat|head|tail|less|sed\s+-n|bat)\b/,   "Leyendo un fichero"],
  [/\b(curl|wget|http)\b/,                    "Llamando a una API"],
  [/\bdocker\b|\bkubectl\b|\bterraform\b/, "Tocando infraestructura"],
  [/\bmosquitto|mqtt/,                         "Hablando con MQTT"],
  [/\b(python3?|node|bun|deno|ruby|php)\s/,    "Ejecutando un script"],
  [/\b(mkdir|cp|mv|rm|touch|chmod)\b/,        "Moviendo ficheros"],
  [/\b(ssh|scp|rsync)\b/,                     "Conectando a otra maquina"],
  [/\becho\b|\bprintf\b/,                    "Escribiendo texto"],
];

function describeBash(cmd: string): string {
  const c = cmd.toLowerCase();
  for (const [re, txt] of BASH) if (re.test(c)) return txt;
  // Sin receta: al menos el programa, que ya orienta.
  const prog = cmd.trim().split(/\s+/)[0]?.split("/").pop() ?? "";
  return prog ? `Ejecutando ${prog}` : "Ejecutando un comando";
}

/** "mcp__plugin_playwright_playwright__browser_take_screenshot" -> "Playwright: browser take screenshot" */
function describeMcp(tool: string): string {
  const parts = tool.split("__").filter(Boolean);
  const accion = (parts.at(-1) ?? "").replace(/_/g, " ");
  const server = (parts.at(-2) ?? "").split("_").filter(Boolean).at(-1) ?? "MCP";
  const cap = server.charAt(0).toUpperCase() + server.slice(1);
  return accion ? `${cap}: ${accion}` : cap;
}

export function describe(tool: string, input: Record<string, unknown>): string {
  const pick = (k: string) => (typeof input[k] === "string" ? (input[k] as string) : undefined);
  const file = () => basename(pick("file_path") ?? "") || "un fichero";

  if (tool.startsWith("mcp__")) return truncate(describeMcp(tool), 40);

  switch (tool) {
    case "Bash":       return truncate(describeBash(pick("command") ?? ""), 40);
    case "Read":       return truncate(`Leyendo ${file()}`, 40);
    case "Edit":       return truncate(`Editando ${file()}`, 40);
    case "Write":      return truncate(`Escribiendo ${file()}`, 40);
    case "NotebookEdit": return truncate(`Editando ${file()}`, 40);
    case "Grep":       return truncate(`Buscando "${pick("pattern") ?? ""}"`, 40);
    case "Glob":       return truncate(`Buscando ficheros ${pick("pattern") ?? ""}`, 40);
    case "WebFetch":   {
      let host = pick("url") ?? "";
      try { host = new URL(host).hostname.replace(/^www\./, ""); } catch { /* no es una URL */ }
      return truncate(`Leyendo ${host}`, 40);
    }
    case "WebSearch":  return truncate(`Buscando en la web: ${pick("query") ?? ""}`, 40);
    case "Task":
    case "Agent":      return truncate(`Lanzando un subagente: ${pick("description") ?? ""}`, 40);
    case "TodoWrite":  return "Actualizando su lista de tareas";
    case "Skill":      return truncate(`Usando la skill ${pick("skill") ?? ""}`, 40);
    case "Artifact":   return "Publicando un artifact";
    default:           return truncate(tool, 40);
  }
}

/** Primera linea util de la invocacion, para que se lea de un vistazo en la placa. */
export function summarize(tool: string, input: Record<string, unknown>): string | null {
  const pick = (k: string) => (typeof input[k] === "string" ? (input[k] as string) : undefined);

  switch (tool) {
    case "Bash":      return truncate(pick("command") ?? "");
    case "Read":
    case "Write":
    case "Edit":      return truncate(basename(pick("file_path") ?? ""));
    case "Glob":      return truncate(pick("pattern") ?? "");
    case "Grep":      return truncate(pick("pattern") ?? "");
    case "WebFetch":  return truncate(pick("url") ?? "");
    case "WebSearch": return truncate(pick("query") ?? "");
    case "Task":
    case "Agent":     return truncate(pick("description") ?? pick("subagent_type") ?? "");
    default: {
      const first = Object.values(input).find((v) => typeof v === "string") as string | undefined;
      return first ? truncate(first) : null;
    }
  }
}

/** Estado propio de la sesion, sin contar subagentes. */
const own = new Map<string, AgentState>();

/** Nombre base de cada sesion, antes de desambiguar. */
const bases = new Map<string, string>();

/**
 * Dos sesiones pueden llamarse igual —es normal tener dos "brain" abiertas— y
 * entonces la pantalla no sirve: una decia "te espera" y la otra estaba
 * trabajando, sin forma de saber cual era cual. Cuando el nombre se repite se
 * le pega el principio del id a TODAS las que lo compartan.
 */
function distinguir(sid: string, base: string): string {
  bases.set(sid, base);

  let repetido = false;
  for (const [otro, b] of bases) {
    if (otro !== sid && b === base && own.has(otro)) { repetido = true; break; }
  }
  return repetido ? `${base} #${sid.slice(0, 4)}` : base;
}

/** Cuando aparece un nombre repetido hay que repintar tambien al otro. */
function republishHermanas(sid: string) {
  const base = bases.get(sid);
  if (!base) return;
  for (const [otro, b] of bases) {
    if (otro !== sid && b === base && own.has(otro)) republish(otro, true);
  }
}

/**
 * Lo que se publica es la sesion FUNDIDA con sus subagentes: gana el estado
 * mas interesante de los dos lados. Un padre en reposo con un subagente
 * trabajando esta trabajando, que es lo que el humano necesita ver; antes
 * salia "en reposo" y mentia.
 *
 * error y waiting_permission del PADRE mandan siempre: son cosas que te
 * reclaman a ti, no trabajo de fondo.
 */
function republish(sid: string, sinHermanas = false): AgentState {
  const base = own.get(sid)!;
  const ss = liveSubs(sid);

  // La etiqueta se recalcula aqui, no solo al crear la sesion: el titulo se
  // lee del transcript en segundo plano y llega despues del primer evento.
  // Sin esto el nombre bueno se guardaba y nunca se llegaba a publicar.
  const titulo = titles.get(sid);
  let merged: AgentState = {
    ...base,
    label: distinguir(sid, titulo ? shortLabel(titulo) : base.label),
    subN: ss.length,
    subType: null,
  };

  if (ss.length && base.status !== "error" && base.status !== "waiting_permission") {
    const best = ss.reduce((a, b) => (RANK[a.status] <= RANK[b.status] ? a : b));
    if (RANK[best.status] < RANK[base.status]) {
      merged = {
        ...merged,
        status: best.status,
        tool: best.tool,
        detail: best.detail,
        raw: best.raw,
        subType: best.type,
      };
    }
  }

  // Con subagentes arrancados, la sesion NO esta en reposo aunque su propio
  // turno haya terminado: esta esperandolos. Es lo que dice el terminal.
  const esperando = pending.get(sid)?.size ?? 0;
  if (esperando > 0 && (merged.status === "idle" || merged.status === "offline")) {
    merged.status = "working";
    if (!merged.detail) {
      merged.detail = esperando === 1
        ? "Esperando a un subagente"
        : `Esperando a ${esperando} subagentes`;
      merged.tool = null;
    }
  }
  merged.subN = Math.max(merged.subN ?? 0, esperando);

  // Si la sesion corre en un panel de tmux, un boton de la placa escribe en
  // la conversacion viva. Si no, lo unico posible es abrir una sesion nueva,
  // y eso hay que decirlo ANTES de pulsar, no despues.
  merged.tmux = hasPaneFor(cwds.get(sid));
  merged.awaiting = base.awaiting === true && merged.status === "idle";

  const prev = agents.get(sid);
  // 'since' solo se reinicia cuando cambia el estado: es el cronometro de la UI.
  merged.since = prev && prev.status === merged.status ? prev.since : now();

  agents.set(sid, merged);
  publishAgentState(merged);
  if (!sinHermanas) republishHermanas(sid);
  return merged;
}

function upsert(h: HookBase, patch: Partial<AgentState>): AgentState {
  const id  = agentIdOf(h);
  const sub = subIdOf(h);

  if (h.cwd) cwds.set(id, h.cwd);
  rememberTitle(id, h.transcript_path);

  if (sub) {
    // Evento de un subagente: no toca el estado propio de la sesion.
    if (!subs.has(id)) subs.set(id, new Map());
    subs.get(id)!.set(sub, {
      type: h.agent_type ?? "subagente",
      status: patch.status ?? "working",
      tool: patch.tool ?? null,
      detail: patch.detail ?? null,
      raw: patch.raw ?? null,
      rx: Date.now(),
    });
  } else {
    const prev = own.get(id);
    own.set(id, {
      id,
      awaiting: false,          // si pasa cualquier otra cosa, ya no te espera
      label: labelOf(h, id),
      status: patch.status ?? prev?.status ?? "idle",
      tool: patch.tool !== undefined ? patch.tool : (prev?.tool ?? null),
      detail: patch.detail !== undefined ? patch.detail : (prev?.detail ?? null),
      raw: patch.raw !== undefined ? patch.raw : (prev?.raw ?? null),
      since: prev?.since ?? now(),
      lastError: patch.lastError !== undefined ? patch.lastError : (prev?.lastError ?? null),
    });
  }

  if (!own.has(id)) {
    // Llego antes un subagente que el SessionStart del padre.
    own.set(id, {
      id, label: labelOf(h, id), status: "idle",
      tool: null, detail: null, raw: null, since: now(), lastError: null,
    });
  }

  const t = reapers.get(id);
  if (t) { clearTimeout(t); reapers.delete(id); }

  return republish(id);
}

/** Un subagente ha terminado: fuera del grupo y se repinta el padre. */
export function onSubagentStop(h: HookBase): AgentState | null {
  const id = agentIdOf(h), sub = subIdOf(h);
  if (!sub) return null;
  subs.get(id)?.delete(sub);
  pending.get(id)?.delete(sub);
  if (!own.has(id)) return null;
  return republish(id);
}

export function onSessionStart(h: HookBase) {
  return upsert(h, { status: "idle", tool: null, detail: null, lastError: null });
}

export function onPreTool(h: HookPreTool) {
  return upsert(h, {
    status: "working",
    tool: h.tool_name,
    detail: describe(h.tool_name, h.tool_input ?? {}),
    raw: summarize(h.tool_name, h.tool_input ?? {}),
  });
}

/**
 * Le has mandado algo: esta pensando, aunque todavia no haya tocado ninguna
 * herramienta. Sin esto habia un hueco —a veces largo— entre que escribes y
 * que empieza a trabajar, y la pantalla decia "en reposo" mintiendo.
 */
export function onUserPrompt(h: HookBase & { prompt?: string }) {
  const p = (h.prompt ?? "").replace(/\s+/g, " ").trim();
  return upsert(h, {
    status: "thinking",
    tool: null,
    detail: p ? truncate(`Le has pedido: ${p}`, 46) : "Pensando",
    raw: p ? truncate(p, 64) : null,
  });
}

/**
 * Stop no es "parado": es "he terminado de contestar y la pelota esta en tu
 * tejado". Es la unica señal fiable de que una sesion te espera A TI, y no la
 * misma que estar simplemente quieta —una recuperada del disco esta quieta,
 * pero no sabemos si quiere algo—.
 *
 * El evento trae el ultimo mensaje del asistente, asi que la tarjeta puede
 * decir sobre QUE te espera en vez de solo que espera.
 */
export function onStop(h: HookBase & { last_assistant_message?: string }) {
  const dicho = (h.last_assistant_message ?? "").replace(/\s+/g, " ").trim();
  const st = upsert(h, {
    status: "idle",
    tool: null,
    detail: dicho ? truncate(dicho, 60) : null,
    raw: dicho ? truncate(dicho, 64) : null,
  });
  const o = own.get(agentIdOf(h));
  if (o) { o.awaiting = true; }
  return republish(agentIdOf(h));
}

export function onStopFailure(h: HookStopFailure) {
  return upsert(h, { status: "error", tool: null, detail: h.error_type, lastError: h.error_type });
}

export function onSessionEnd(h: HookBase) {
  const s = upsert(h, { status: "offline", tool: null, detail: null });

  // El retained de un agente muerto no puede quedarse para siempre.
  const timer = setTimeout(() => {
    clearAgentState(s.id);
    agents.delete(s.id);
    own.delete(s.id);
    bases.delete(s.id);
    subs.delete(s.id);
    pending.delete(s.id);
    cwds.delete(s.id);
    titles.delete(s.id);
    reapers.delete(s.id);
    console.log(`[state] ${s.id} purgado tras ${OFFLINE_TTL_MS / 60000} min offline`);
  }, OFFLINE_TTL_MS);
  timer.unref?.();
  reapers.set(s.id, timer);

  return s;
}

export function setWaitingPermission(agentId: string, tool: string, summary: string) {
  const prev = own.get(agentId);
  if (!prev) return;
  own.set(agentId, { ...prev, status: "waiting_permission", tool, detail: summary });
  republish(agentId);
}

/** Saca al agente de waiting_permission cuando la peticion se resuelve. */
export function clearWaitingPermission(agentId: string) {
  const prev = own.get(agentId);
  if (!prev || prev.status !== "waiting_permission") return;
  own.set(agentId, { ...prev, status: "working" });
  republish(agentId);
}

/**
 * Reconstruye las sesiones leyendo los transcripts de Claude Code.
 *
 * Sin esto, un reinicio del broker borraba el estado y una sesion solo
 * reaparecia cuando emitia un evento. Las que estan paradas esperandote no
 * emiten nada, asi que desaparecian de la pantalla justo las que mas te
 * interesa ver. Es el caso que destapo "tengo una sesion de brain que no sale".
 *
 * Se toma la fecha del fichero como ultima actividad, asi que una sesion
 * aparcada hace media hora entra directamente como "te espera", que es la
 * verdad.
 */
export async function seedFromDisk(maxAgeH = 12): Promise<number> {
  const home = process.env.HOME ?? "";
  const glob = new Bun.Glob("projects/*/*.jsonl");
  const corte = Date.now() - maxAgeH * 3600_000;
  let n = 0;

  for await (const rel of glob.scan({ cwd: `${home}/.claude`, absolute: true })) {
    try {
      const f = Bun.file(rel);
      const mtime = (await f.stat()).mtimeMs;
      if (mtime < corte) continue;

      const base = rel.replace(/\.jsonl$/, "");
      const id = (base.split("/").pop() ?? "").replace(/-/g, "").slice(0, 8);
      if (!id || own.has(id)) continue;

      // El cwd esta en las primeras lineas; no hace falta leer el fichero entero.
      let cwd: string | undefined;
      const cabeza = await f.slice(0, 64 * 1024).text();
      for (const line of cabeza.split("\n")) {
        if (!line.includes('"cwd"')) continue;
        try { cwd = (JSON.parse(line) as { cwd?: string }).cwd; } catch { /* a medias */ }
        if (cwd) break;
      }

      const titulo = await readCustomTitle(rel);
      if (titulo) titles.set(id, titulo);
      if (cwd) cwds.set(id, cwd);

      const carpeta = cwd ? (cwd.split("/").filter(Boolean).at(-1) ?? id) : id;
      own.set(id, {
        id,
        // Recuperada del disco: esta quieta, pero no hay forma de saber si
        // espera algo. Quieta y esperandote no son lo mismo.
        awaiting: false,
        label: shortLabel(titulo ?? carpeta),
        status: "idle",
        tool: null,
        detail: null,
        raw: null,
        since: Math.floor(mtime / 1000),
        lastError: null,
      });
      republish(id);
      n++;
    } catch { /* un transcript ilegible no puede tumbar el arranque */ }
  }
  return n;
}

export function agentIdFor(h: HookBase): string { return agentIdOf(h); }
export function snapshot(): AgentState[] { return [...agents.values()]; }
