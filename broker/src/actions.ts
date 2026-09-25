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

/**
 * Rutas de los binarios. launchd arranca con un PATH minimo
 * (/usr/bin:/bin:/usr/sbin:/sbin) que NO incluye /opt/homebrew/bin, asi que
 * buscar "tmux" a secas falla solo cuando corre como servicio, que es como
 * corre de verdad.
 */
const TMUX   = process.env.TMUX_BIN   ?? "tmux";
const CLAUDE = process.env.CLAUDE_BIN ?? "claude";

export type ActionId = "go" | "continue" | "tests" | "status" | "interrupt";

interface ActionDef {
  label: string;
  prompt: string;          // lo que se escribe en la sesion
  tmuxOnly?: boolean;      // interrupt no tiene equivalente headless
}

const CATALOG: Record<ActionId, ActionDef> = {
  // El caso mas comun de todos: la sesion ha terminado de explicar algo y
  // solo espera un "tira". Va primero y con el texto mas corto posible.
  go:        { label: "ADELANTE", prompt: "adelante" },
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
  try {
    const p = Bun.spawn(cmd, { cwd: opts.cwd, stdout: "pipe", stderr: "pipe" });
    const [out, err] = await Promise.all([
      new Response(p.stdout).text(),
      new Response(p.stderr).text(),
    ]);
    return { code: await p.exited, out, err };
  } catch (e) {
    // Bun.spawn LANZA si el ejecutable no existe. Sin esto la excepcion sube
    // por el handler async, nadie la recoge y el proceso entero se muere: un
    // boton de la placa tumbaba el broker y launchd lo resucitaba en bucle.
    return { code: 127, out: "", err: (e as Error).message };
  }
}

/**
 * Un campo por llamada, emparejados por numero de linea.
 *
 * Antes se pedian los tres de golpe separados por un caracter, y el separador
 * se perdia entre el fuente y tmux: los tres campos llegaban pegados, ningun
 * panel casaba nunca y TODA accion caia a claude -p en silencio. Probado con
 * tabulador y con \x1f, y fallaba igual. Pedirlos por separado no tiene
 * separador que perder.
 *
 * tmux lista los paneles en el mismo orden en llamadas consecutivas. Si
 * cambiaran entre medias, las listas no cuadran en longitud y se descartan:
 * es preferible no encontrar panel a escribir en el equivocado.
 */
/**
 * Cache de paneles. Saber si una sesion es alcanzable por tmux hace falta en
 * CADA publicacion de estado, y lanzar tres procesos por evento seria absurdo.
 * Se refresca sola cada diez segundos.
 */
let paneCache: Pane[] = [];

export async function refreshPanes() {
  paneCache = await listPanes();
}

/** Sincrono a proposito: lo consulta el camino de publicacion. */
export function hasPaneFor(cwdRaw: string | undefined): boolean {
  if (!cwdRaw) return false;
  const cwd = realOrSame(cwdRaw);
  return paneCache.some((p) => realOrSame(p.path) === cwd);
}

async function listPanes(): Promise<Pane[]> {
  const campos = ["#{session_name}:#{window_index}.#{pane_index}",
                  "#{pane_current_path}",
                  "#{pane_current_command}"];

  const salidas: string[][] = [];
  for (const f of campos) {
    const { code, out, err } = await run([TMUX, "list-panes", "-a", "-F", f]);
    if (code === 127) {
      console.warn(`[act ] tmux no ejecutable (${err.trim()}); solo quedara claude -p`);
      return [];
    }
    if (code !== 0) return [];          // sin servidor de tmux: normal
    salidas.push(out.split("\n").filter((l) => l.length > 0));
  }

  const [targets = [], paths = [], commands = []] = salidas;
  if (targets.length !== paths.length || targets.length !== commands.length) {
    console.warn("[act ] los paneles cambiaron entre llamadas, descarto la lista");
    return [];
  }

  return targets.map((target, i) => ({
    target,
    path: paths[i] ?? "",
    command: commands[i] ?? "",
  }));
}

/**
 * Las rutas hay que resolverlas antes de compararlas. En macOS /tmp es un
 * symlink a /private/tmp: el hook reporta una y tmux la otra.
 */
function realOrSame(p: string): string {
  try { return realpathSync(p); } catch { return p; }
}

/**
 * Un panel sirve si su cwd coincide con el del agente. Se prefiere uno que
 * ademas parezca estar corriendo Claude Code: si tienes dos paneles en el
 * mismo repo, escribir en el shell equivocado ejecutaria la frase como un
 * comando.
 */
async function findPane(cwdRaw: string): Promise<Pane | null> {
  const cwd = realOrSame(cwdRaw);
  const panes = await listPanes();
  if (process.env.ACTIONS_DEBUG === "1") {
    console.log(`[act ] busco "${cwd}" entre ${panes.length} paneles: ` +
      panes.map((p) => `${p.target}=${realOrSame(p.path)}(${p.command})`).join(", "));
  }
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
    console.log(`[act ] ${agentId} ${action} -> SIN CWD conocido`);
    return { ok: false, via: "none", detail: "no se conoce el directorio del agente" };
  }

  const pane = await findPane(cwd);

  if (pane) {
    if (DRY_RUN) {
      return { ok: true, via: "tmux", detail: `[dry-run] ${pane.target} <- ${action === "interrupt" ? "Escape" : def.prompt}` };
    }
    const keys = action === "interrupt"
      ? [TMUX, "send-keys", "-t", pane.target, "Escape"]
      : [TMUX, "send-keys", "-t", pane.target, def.prompt, "Enter"];
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
  try {
    const p = Bun.spawn([CLAUDE, "-p", def.prompt], {
      cwd,
      stdout: "ignore",
      stderr: "ignore",
      stdin: "ignore",
    });
    p.unref();
    console.log(`[act ] ${agentId} ${action} -> claude -p en ${cwd} (pid ${p.pid})`);
    return { ok: true, via: "claude-p", detail: "sesion nueva, sin contexto" };
  } catch (e) {
    const msg = (e as Error).message;
    console.warn(`[act ] ${agentId} ${action} -> no se pudo lanzar claude: ${msg}`);
    return { ok: false, via: "none", detail: `claude no ejecutable: ${msg.slice(0, 60)}` };
  }
}
