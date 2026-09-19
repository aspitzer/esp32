import { randomUUID } from "node:crypto";
import type { PermRequest, PermResponse } from "./types.ts";
import { publishPermRequest, onPermResponse, anyDeviceOnline } from "./mqtt.ts";
import { setWaitingPermission, clearWaitingPermission } from "./state.ts";

/**
 * Registro de peticiones de permiso abiertas.
 *
 * Regla que no se negocia: este modulo SIEMPRE resuelve. Nunca deja que el hook
 * agote su timeout, porque eso congelaria la sesion de Claude Code. Si la placa
 * no contesta en PERM_TIMEOUT_MS, se resuelve como "unspecified" y el flujo
 * normal de permisos sigue su curso.
 */

const TIMEOUT_MS = Number(process.env.PERM_TIMEOUT_MS ?? 90_000);

/**
 * Solo se molesta al humano por encima de este nivel. Todo lo que quede por
 * debajo se resuelve como "unspecified" al instante y sigue el flujo normal
 * de permisos en el portatil. Sin esto la placa pita por cada Edit y dejas
 * de mirarla, que es el fracaso real del proyecto.
 */
const MIN_RISK = (process.env.PERM_MIN_RISK ?? "high") as PermRequest["risk"];
const RISK_ORDER: Record<PermRequest["risk"], number> = { low: 0, medium: 1, high: 2 };

export type Verdict = "allow" | "deny" | "unspecified";

interface Pending {
  req: PermRequest;
  resolve: (v: { verdict: Verdict; source: string }) => void;
  timer: ReturnType<typeof setTimeout>;
}

const pending = new Map<string, Pending>();

onPermResponse((res: PermResponse) => {
  const p = pending.get(res.requestId);
  if (!p) {
    console.warn(`[perm] respuesta para ${res.requestId} desconocida (tarde o duplicada)`);
    return;
  }
  clearTimeout(p.timer);
  pending.delete(res.requestId);
  console.log(`[perm] ${res.requestId} -> ${res.decision} (${res.source})`);
  // Sin esto el agente se queda en waiting_permission hasta el siguiente
  // PreToolUse. Si lo has denegado, ese evento no llega nunca.
  clearWaitingPermission(p.req.agentId);
  p.resolve({ verdict: res.decision, source: res.source });
});

/** Heuristica de riesgo. Conservadora: ante la duda, sube el nivel. */
export function riskOf(tool: string, summary: string): PermRequest["risk"] {
  const s = summary.toLowerCase();
  const destructive =
    /\brm\s+-[rf]|--force|-f\b|\bdrop\s+(table|database)\b|\btruncate\b|\bgit\s+push\b|\bgit\s+reset\s+--hard\b|\bchmod\s+777\b|\bcurl\b.*\|\s*(ba)?sh|\bmkfs\b|\bdd\s+if=/.test(s);
  if (destructive) return "high";
  if (tool === "Bash") return "medium";
  if (tool === "Write" || tool === "Edit") return "medium";
  return "low";
}

export function pendingCount(): number { return pending.size; }
export function minRisk(): string { return MIN_RISK; }

/**
 * Pide una decision a la placa. Resuelve siempre, como muy tarde en TIMEOUT_MS.
 * Si no hay placa viva, devuelve "unspecified" de inmediato.
 */
export function ask(
  agentId: string,
  tool: string,
  summary: string,
): Promise<{ verdict: Verdict; source: string }> {
  const risk = riskOf(tool, summary);

  if (RISK_ORDER[risk] < RISK_ORDER[MIN_RISK]) {
    return Promise.resolve({ verdict: "unspecified", source: `risk:${risk}` });
  }

  if (!anyDeviceOnline()) {
    console.log("[perm] ninguna placa online -> unspecified (fallback inmediato)");
    return Promise.resolve({ verdict: "unspecified", source: "fallback" });
  }

  const req: PermRequest = {
    requestId: randomUUID(),
    agentId,
    tool,
    summary,
    risk,
    expiresAt: Math.floor((Date.now() + TIMEOUT_MS) / 1000),
  };

  return new Promise((resolve) => {
    const timer = setTimeout(() => {
      pending.delete(req.requestId);
      console.log(`[perm] ${req.requestId} sin respuesta en ${TIMEOUT_MS} ms -> unspecified`);
      clearWaitingPermission(agentId);
      resolve({ verdict: "unspecified", source: "timeout" });
    }, TIMEOUT_MS);
    timer.unref?.();

    pending.set(req.requestId, { req, resolve, timer });
    setWaitingPermission(agentId, tool, summary);
    publishPermRequest(req);
    console.log(`[perm] ${req.requestId} ${agentId} ${tool} riesgo=${req.risk}: ${summary}`);
  });
}
