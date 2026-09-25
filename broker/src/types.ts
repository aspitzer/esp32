// Tipos del contrato MQTT. Ver ../../docs/mqtt-contract.md
// Cambiar esto obliga a cambiar el firmware en el mismo commit.

export type AgentStatus =
  | "idle"
  | "thinking"
  | "working"
  | "waiting_permission"
  | "error"
  | "offline";

export interface AgentState {
  id: string;
  label: string;
  status: AgentStatus;
  tool: string | null;
  detail: string | null;
  since: number;            // epoch en segundos
  lastError: string | null;
  raw?: string | null;      // el comando literal, para la pantalla de detalle
  subN?: number;            // subagentes activos bajo esta sesion
  subType?: string | null;  // tipo del subagente que manda ahora mismo
}

export interface PermRequest {
  requestId: string;
  agentId: string;
  tool: string;
  summary: string;
  risk: "low" | "medium" | "high";
  expiresAt: number;        // epoch en segundos
}

export interface ActionRequest {
  agentId: string;
  action: string;          // validado contra el catalogo cerrado de actions.ts
  source?: string;
}

export interface ActionResponse {
  agentId: string;
  action: string;
  ok: boolean;
  via: string;
  detail: string;
}

export interface PermResponse {
  requestId: string;
  decision: "allow" | "deny";
  source: "device" | "timeout" | "fallback";
}

// --- Entrada de los hooks de Claude Code -------------------------------------
// https://code.claude.com/docs/en/hooks

export interface HookBase {
  session_id: string;
  transcript_path?: string;
  cwd?: string;
  hook_event_name: string;
  permission_mode?: string;
  agent_id?: string;        // solo en contexto de subagente
  agent_type?: string;
}

export interface HookPreTool extends HookBase {
  tool_name: string;
  tool_input: Record<string, unknown>;
  tool_use_id?: string;
}

export interface HookStopFailure extends HookBase {
  error_type: string;
}
