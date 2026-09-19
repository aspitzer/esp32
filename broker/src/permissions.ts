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

/**
 * Heuristica de riesgo. Conservadora, pero calibrada: si marca alto lo que no
 * lo es, la placa interrumpe por todo y dejas de mirarla. "git push" a secas
 * no destruye nada; "git push --force" reescribe historia publica. Un `-f`
 * suelto tampoco vale como senal: `grep -f patrones.txt` es inofensivo.
 */
const HIGH = [
  /\brm\s+-[a-z]*[rf]/,                  // rm -rf, rm -fr, rm -Rf
  /\bgit\s+push\b[^|;]*\s(--force\b|-f\b)/, // push forzado, no un push normal
  /\bgit\s+push\b[^|;]*--force-with-lease/,
  /\bgit\s+reset\s+--hard\b/,
  /\bgit\s+clean\s+-[a-z]*[fd]/,
  /\bdrop\s+(table|database|schema)\b/,
  /\btruncate\s+table\b/,
  /\bdelete\s+from\b(?![^;]*\bwhere\b)/,  // DELETE sin WHERE
  /\bmkfs\b|\bdd\s+if=/,
  /\bchmod\s+(-[a-z]+\s+)?777\b/,
  /\b(curl|wget)\b[^|]*\|\s*(sudo\s+)?(ba|z|d)?sh\b/,  // descargar y ejecutar
  /\bsudo\b/,
  /\bkubectl\s+delete\b|\bterraform\s+(destroy|apply)\b/,
  /\bdocker\s+(system\s+prune|rm\s+-f)\b/,
  /\bnpm\s+publish\b|\bbun\s+publish\b/,
];

export function riskOf(tool: string, summary: string): PermRequest["risk"] {
  const s = summary.toLowerCase();
  if (HIGH.some((re) => re.test(s))) return "high";
  if (tool === "Bash" || tool === "Write" || tool === "Edit") return "medium";
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
