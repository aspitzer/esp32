import { basename } from "node:path";
import type { AgentState, AgentStatus, HookBase, HookPreTool, HookStopFailure } from "./types.ts";
import { publishAgentState, clearAgentState } from "./mqtt.ts";

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
  rx: number;              // ms, para caducar subagentes que mueren sin avisar
}

/** sessionId -> subagentes vivos */
const subs = new Map<string, Map<string, Sub>>();

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
 * Titulo de la sesion que Claude Code genera solo y guarda en el transcript
 * como "aiTitle". Es muchisimo mas util que el nombre de la carpeta: dos
 * sesiones en el mismo repo son indistinguibles por carpeta, y "civitatis-work"
 * dice menos que "Revenue tracking en business case".
 */
const titles = new Map<string, string>();
const titleRead = new Map<string, number>();
const TITLE_REFRESH_MS = 5 * 60 * 1000;

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
  const last = titleRead.get(id) ?? 0;
  if (Date.now() - last < TITLE_REFRESH_MS) return;
  titleRead.set(id, Date.now());

  void readTitle(path).then((t) => {
    if (t && titles.get(id) !== t) {
      titles.set(id, t);
      if (own.has(id)) republish(id);      // repinta con el nombre bueno
    }
  });
}

/** Recorta a lo que cabe en una fila, ya en ASCII. */
function shortLabel(raw: string): string {
  const l = toAscii(raw);
  return l.length > 28 ? l.slice(0, 27) + "..." : l;
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

/**
 * Lo que se publica es la sesion FUNDIDA con sus subagentes: gana el estado
 * mas interesante de los dos lados. Un padre en reposo con un subagente
 * trabajando esta trabajando, que es lo que el humano necesita ver; antes
 * salia "en reposo" y mentia.
 *
 * error y waiting_permission del PADRE mandan siempre: son cosas que te
 * reclaman a ti, no trabajo de fondo.
 */
function republish(sid: string): AgentState {
  const base = own.get(sid)!;
  const ss = liveSubs(sid);

  // La etiqueta se recalcula aqui, no solo al crear la sesion: el titulo se
  // lee del transcript en segundo plano y llega despues del primer evento.
  // Sin esto el nombre bueno se guardaba y nunca se llegaba a publicar.
  const titulo = titles.get(sid);
  let merged: AgentState = {
    ...base,
    label: titulo ? shortLabel(titulo) : base.label,
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
        subType: best.type,
      };
    }
  }

  const prev = agents.get(sid);
  // 'since' solo se reinicia cuando cambia el estado: es el cronometro de la UI.
  merged.since = prev && prev.status === merged.status ? prev.since : now();

  agents.set(sid, merged);
  publishAgentState(merged);
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
      rx: Date.now(),
    });
  } else {
    const prev = own.get(id);
    own.set(id, {
      id,
      label: labelOf(h, id),
      status: patch.status ?? prev?.status ?? "idle",
      tool: patch.tool !== undefined ? patch.tool : (prev?.tool ?? null),
      detail: patch.detail !== undefined ? patch.detail : (prev?.detail ?? null),
      since: prev?.since ?? now(),
      lastError: patch.lastError !== undefined ? patch.lastError : (prev?.lastError ?? null),
    });
  }

  if (!own.has(id)) {
    // Llego antes un subagente que el SessionStart del padre.
    own.set(id, {
      id, label: labelOf(h, id), status: "idle",
      tool: null, detail: null, since: now(), lastError: null,
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
    detail: summarize(h.tool_name, h.tool_input ?? {}),
  });
}

export function onStop(h: HookBase) {
  return upsert(h, { status: "idle", tool: null, detail: null });
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
    subs.delete(s.id);
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

export function agentIdFor(h: HookBase): string { return agentIdOf(h); }
export function snapshot(): AgentState[] { return [...agents.values()]; }
