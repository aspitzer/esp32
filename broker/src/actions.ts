/**
 * Fase 4 — acciones enlatadas desde la placa.
 *
 * Dos caminos, elegidos por agente:
 *
 *   tmux    Si la sesion corre dentro de tmux, send-keys escribe DE VERDAD en
 *           la conversacion viva. Es el unico camino que continua el hilo.
 *   claude -p   Si no, se lanza un proceso headless en el mismo directorio.
 *           NO es la misma conversacion y no tiene el contexto: sirve para
 *           "lanza los tests", no para "continua".
 *
 * SEGURIDAD: el catalogo de acciones es CERRADO y el texto sale de aqui, nunca
 * del mensaje MQTT. Todo se lanza con argv (sin shell), asi que ni el agentId
 * ni nada que venga de la red puede inyectar comandos. Un dispositivo
 * comprometido en la LAN solo puede disparar una de estas cuatro frases.
 */

import { realpathSync } from "node:fs";
import { cwdOf } from "./state.ts";

/**
 * ACTIONS_DRY_RUN=1 informa del camino elegido sin ejecutar nada. Existe
 * porque probar el camino real de claude -p lanza una sesion de verdad, que
 * gasta tokens y hace trabajo: no es algo que se pueda dejar en una bateria
 * de pruebas.
 */
const DRY_RUN = process.env.ACTIONS_DRY_RUN === "1";

export type ActionId = "continue" | "tests" | "status" | "interrupt";

interface ActionDef {
  label: string;
  prompt: string;          // lo que se escribe en la sesion
  tmuxOnly?: boolean;      // interrupt no tiene equivalente headless
}

const CATALOG: Record<ActionId, ActionDef> = {
  continue:  { label: "CONTINUA", prompt: "continúa" },
  tests:     { label: "TESTS",    prompt: "lanza los tests y arregla lo que falle" },
  status:    { label: "ESTADO",   prompt: "resume en tres lineas donde estas y que falta" },
  interrupt: { label: "PARAR",    prompt: "", tmuxOnly: true },
};

export function isActionId(x: unknown): x is ActionId {
  return typeof x === "string" && Object.hasOwn(CATALOG, x);
}

export function actionCatalog() {
  return Object.entries(CATALOG).map(([id, d]) => ({ id, label: d.label, tmuxOnly: !!d.tmuxOnly }));
}

// --- tmux --------------------------------------------------------------------

interface Pane {
  target: string;      // session:window.pane
  path: string;
  command: string;
}

async function run(cmd: string[], opts: { cwd?: string } = {}) {
  const p = Bun.spawn(cmd, { cwd: opts.cwd, stdout: "pipe", stderr: "pipe" });
  const [out, err] = await Promise.all([
    new Response(p.stdout).text(),
    new Response(p.stderr).text(),
  ]);
  return { code: await p.exited, out, err };
}

async function listPanes(): Promise<Pane[]> {
  const { code, out } = await run([
    "tmux", "list-panes", "-a", "-F",
    "#{session_name}:#{window_index}.#{pane_index}\t#{pane_current_path}\t#{pane_current_command}",
  ]);
  if (code !== 0) return [];            // tmux no instalado o sin servidor: normal
  return out
    .split("\n")
    .filter(Boolean)
    .map((line) => {
      const [target = "", path = "", command = ""] = line.split("\t");
      return { target, path, command };
    });
}

/**
 * Las rutas hay que resolverlas antes de compararlas. En macOS /tmp es un
 * symlink a /private/tmp: el hook reporta una y tmux la otra, y la comparacion
 * literal no casa nunca. Lo mismo pasa con /var y con cualquier repo bajo un
 * enlace.
 */
function realOrSame(p: string): string {
  try { return realpathSync(p); } catch { return p; }
}

/**
 * Un panel sirve si su cwd coincide con el del agente. Se prefiere uno que
 * ademas parezca estar corriendo Claude Code: si tienes dos paneles en el
 * mismo repo, escribir en el shell equivocado seria ejecutar la frase como
 * un comando.
 */
async function findPane(cwdRaw: string): Promise<Pane | null> {
  const cwd = realOrSame(cwdRaw);
  const panes = await listPanes();
  const sameCwd = panes.filter((p) => realOrSame(p.path) === cwd);
  if (sameCwd.length === 0) return null;
  const looksLikeClaude = sameCwd.find((p) => /claude|node|bun/i.test(p.command));
  return looksLikeClaude ?? sameCwd[0] ?? null;
}

// --- ejecucion ---------------------------------------------------------------

export interface ActionResult {
  ok: boolean;
  via: "tmux" | "claude-p" | "none";
  detail: string;
}

export async function runAction(agentId: string, action: ActionId): Promise<ActionResult> {
  const def = CATALOG[action];
  const cwd = cwdOf(agentId);

  if (!cwd) {
    return { ok: false, via: "none", detail: "no se conoce el directorio del agente" };
  }

  const pane = await findPane(cwd);

  if (pane) {
    if (DRY_RUN) {
      return { ok: true, via: "tmux", detail: `[dry-run] ${pane.target} <- ${action === "interrupt" ? "Escape" : def.prompt}` };
    }
    const keys = action === "interrupt"
      ? ["tmux", "send-keys", "-t", pane.target, "Escape"]
      : ["tmux", "send-keys", "-t", pane.target, def.prompt, "Enter"];
    const { code, err } = await run(keys);
    const ok = code === 0;
    console.log(`[act ] ${agentId} ${action} -> tmux ${pane.target} ${ok ? "OK" : "FALLO " + err.trim()}`);
    return { ok, via: "tmux", detail: ok ? pane.target : err.trim().slice(0, 80) };
  }

  if (def.tmuxOnly) {
    return { ok: false, via: "none", detail: "PARAR solo funciona en tmux" };
  }

  if (DRY_RUN) {
    return { ok: true, via: "claude-p", detail: `[dry-run] claude -p "${def.prompt}" en ${cwd}` };
  }

  // Headless: proceso nuevo, sin el contexto de la conversacion. No se espera
  // a que termine; puede tardar minutos y la placa no debe quedarse colgada.
  const p = Bun.spawn(["claude", "-p", def.prompt], {
    cwd,
    stdout: "ignore",
    stderr: "ignore",
    stdin: "ignore",
  });
  p.unref();
  console.log(`[act ] ${agentId} ${action} -> claude -p en ${cwd} (pid ${p.pid})`);
  return { ok: true, via: "claude-p", detail: "sesion nueva, sin contexto" };
}
