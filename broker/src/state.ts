import { basename } from "node:path";
import type { AgentState, AgentStatus, HookBase, HookPreTool, HookStopFailure } from "./types.ts";
import { publishAgentState, clearAgentState } from "./mqtt.ts";

/** Cuanto se mantiene el retained de un agente muerto antes de borrarlo. */
const OFFLINE_TTL_MS = 10 * 60 * 1000;

const agents = new Map<string, AgentState>();
const reapers = new Map<string, ReturnType<typeof setTimeout>>();

const now = () => Math.floor(Date.now() / 1000);

/**
 * Un agente = una sesion, o un subagente dentro de ella.
 * Los hooks traen agent_id solo en contexto de subagente.
 */
function agentIdOf(h: HookBase): string {
  const sid = h.session_id.replace(/-/g, "").slice(0, 8);
  return h.agent_id ? `${sid}.${h.agent_id.replace(/-/g, "").slice(0, 6)}` : sid;
}

/** Directorios contenedores que no aportan nada como prefijo de la etiqueta. */
const NOISE = new Set([
  "projects", "repos", "repo", "src", "code", "dev", "work", "git",
  "documents", "desktop", "users", "home", "workspace", "sites",
]);

/**
 * "brain/api" se lee mejor que "api", pero "Projects/sherpa" no aporta nada.
 * Se anade el directorio padre solo si no es un contenedor generico y si los
 * dos segmentos caben en el ancho de la fila (24 chars).
 */
function labelOf(h: HookBase): string {
  let dir = "?";
  if (h.cwd) {
    const parts = h.cwd.split("/").filter(Boolean);
    const last = parts.at(-1) ?? "?";
    const parent = parts.at(-2);
    const two = parent ? `${parent}/${last}` : last;
    dir = parent && !NOISE.has(parent.toLowerCase()) && two.length <= 24 ? two : last;
  }
  const label = h.agent_type ? `${dir}:${h.agent_type}` : dir;
  return label.length > 24 ? label.slice(0, 23) + "…" : label;
}

function truncate(s: string, n = 64): string {
  const flat = s.replace(/\s+/g, " ").trim();
  return flat.length > n ? flat.slice(0, n - 1) + "…" : flat;
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

function upsert(h: HookBase, patch: Partial<AgentState>): AgentState {
  const id = agentIdOf(h);
  const prev = agents.get(id);

  const next: AgentState = {
    id,
    label: labelOf(h),
    status: patch.status ?? prev?.status ?? "idle",
    tool: patch.tool !== undefined ? patch.tool : (prev?.tool ?? null),
    detail: patch.detail !== undefined ? patch.detail : (prev?.detail ?? null),
    // 'since' solo se reinicia cuando cambia el estado: es el cronometro de la UI.
    since: !prev || prev.status !== (patch.status ?? prev.status) ? now() : prev.since,
    lastError: patch.lastError !== undefined ? patch.lastError : (prev?.lastError ?? null),
  };

  agents.set(id, next);
  publishAgentState(next);

  const t = reapers.get(id);
  if (t) { clearTimeout(t); reapers.delete(id); }

  return next;
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
    reapers.delete(s.id);
    console.log(`[state] ${s.id} purgado tras ${OFFLINE_TTL_MS / 60000} min offline`);
  }, OFFLINE_TTL_MS);
  timer.unref?.();
  reapers.set(s.id, timer);

  return s;
}

export function setWaitingPermission(agentId: string, tool: string, summary: string) {
  const prev = agents.get(agentId);
  if (!prev) return;
  const next: AgentState = {
    ...prev, status: "waiting_permission", tool, detail: summary, since: now(),
  };
  agents.set(agentId, next);
  publishAgentState(next);
}

export function agentIdFor(h: HookBase): string { return agentIdOf(h); }
export function snapshot(): AgentState[] { return [...agents.values()]; }
