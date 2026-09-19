/**
 * agent-bus — recibe los hooks de Claude Code y publica el estado por MQTT.
 *
 * Escucha SOLO en 127.0.0.1 (los hooks son locales). El puerto MQTT si sale a
 * la LAN, pero eso lo sirve Mosquitto, no este proceso.
 */

import { connect, disconnect, deviceList, purgeStaleAgents } from "./mqtt.ts";
import * as state from "./state.ts";
import { ask, pendingCount } from "./permissions.ts";
import { summarize } from "./state.ts";
import type { HookBase, HookPreTool, HookStopFailure } from "./types.ts";

const HOST  = process.env.HOOK_HOST ?? "127.0.0.1";
const PORT  = Number(process.env.HOOK_PORT ?? 8787);
const TOKEN = process.env.AGENT_BUS_TOKEN;

// Fase 2. Mientras este en false, /hook/permission contesta "unspecified"
// y el flujo normal de permisos de Claude Code sigue su curso.
const PERMISSIONS_ENABLED = process.env.ENABLE_PERMISSIONS === "1";

/** Rutas validas. Una ruta desconocida da 404, no 405: el metodo no es el problema. */
const HOOK_PATHS = [
  "/hook/session-start",
  "/hook/pre-tool",
  "/hook/stop",
  "/hook/stop-failure",
  "/hook/session-end",
  "/hook/permission",
] as const;

/** 200 con cuerpo vacio: recibido, sin decision. Lo que debe devolver la telemetria. */
const ok = () => new Response(null, { status: 200 });

function authorized(req: Request): boolean {
  if (!TOKEN) return true;                       // sin token configurado, no se exige
  return req.headers.get("X-Bus-Token") === TOKEN;
}

async function body<T>(req: Request): Promise<T | null> {
  try { return (await req.json()) as T; } catch { return null; }
}

const server = Bun.serve({
  hostname: HOST,
  port: PORT,
  idleTimeout: 255,          // los hooks de permiso pueden tardar

  async fetch(req) {
    const url = new URL(req.url);

    // Normalizada: sin barra final y en minusculas. "/health/" y "/Health"
    // tienen que llevar al mismo sitio que "/health".
    const path = (url.pathname.replace(/\/+$/, "") || "/").toLowerCase();

    if (path === "/health") {
      return Response.json({
        ok: true,
        agents: state.snapshot(),
        devices: deviceList(),
        pendingPermissions: pendingCount(),
        permissionsEnabled: PERMISSIONS_ENABLED,
      });
    }

    if (path === "/") {
      return Response.json({
        service: "agent-bus",
        endpoints: [
          "GET  /health",
          ...HOOK_PATHS.map((p) => `POST ${p}`),
        ],
      });
    }

    if (!(HOOK_PATHS as readonly string[]).includes(path)) {
      return new Response(`no such endpoint: ${url.pathname}\n`, { status: 404 });
    }

    // A partir de aqui la ruta existe, asi que un 405 significa de verdad
    // que el metodo esta mal y no que la ruta no exista.
    if (req.method !== "POST") {
      return new Response(`${path} solo acepta POST\n`, {
        status: 405,
        headers: { Allow: "POST" },
      });
    }
    if (!authorized(req)) return new Response("forbidden\n", { status: 403 });

    switch (path) {
      case "/hook/session-start": {
        const h = await body<HookBase>(req);
        if (h) log(state.onSessionStart(h));
        return ok();
      }
      case "/hook/pre-tool": {
        const h = await body<HookPreTool>(req);
        if (h) log(state.onPreTool(h));
        return ok();
      }
      case "/hook/stop": {
        const h = await body<HookBase>(req);
        if (h) log(state.onStop(h));
        return ok();
      }
      case "/hook/stop-failure": {
        const h = await body<HookStopFailure>(req);
        if (h) log(state.onStopFailure(h));
        return ok();
      }
      case "/hook/session-end": {
        const h = await body<HookBase>(req);
        if (h) log(state.onSessionEnd(h));
        return ok();
      }

      case "/hook/permission": {
        const h = await body<HookPreTool>(req);
        if (!h) return ok();

        if (!PERMISSIONS_ENABLED) return unspecified("fase 2 no activada");

        const summary = summarize(h.tool_name, h.tool_input ?? {}) ?? h.tool_name;
        const { verdict, source } = await ask(state.agentIdFor(h), h.tool_name, summary);

        if (verdict === "unspecified") return unspecified(`sin decision del dispositivo (${source})`);

        return Response.json({
          hookSpecificOutput: {
            hookEventName: "PermissionRequest",
            decision: {
              verdict,
              reason: `${verdict === "allow" ? "Aprobado" : "Denegado"} desde el dispositivo fisico`,
            },
            systemMessage: `Permiso ${verdict} desde claude-agent-display`,
          },
        });
      }
    }

    return new Response(`no such endpoint: ${url.pathname}\n`, { status: 404 });
  },
});

/**
 * "Sin decision" explicito. La documentacion define verdict:"unspecified" para
 * esto; es mejor que un cuerpo vacio porque deja rastro de que el hook respondio.
 */
function unspecified(reason: string) {
  return Response.json({
    hookSpecificOutput: {
      hookEventName: "PermissionRequest",
      decision: { verdict: "unspecified", reason },
    },
  });
}

function log(s: ReturnType<typeof state.onStop>) {
  console.log(
    `[hook] ${s.id.padEnd(16)} ${s.status.padEnd(18)} ${(s.tool ?? "-").padEnd(10)} ${s.detail ?? ""}`,
  );
}

await connect();
const purged = await purgeStaleAgents();
if (purged) console.log(`[bus ] ${purged} agente(s) fantasma limpiados de un arranque anterior`);

console.log(`[http] hooks en http://${HOST}:${PORT}  (token ${TOKEN ? "exigido" : "NO configurado"})`);
console.log(`[http] permisos ${PERMISSIONS_ENABLED ? "ACTIVADOS" : "desactivados (fase 1)"}`);

for (const sig of ["SIGINT", "SIGTERM"] as const) {
  process.on(sig, () => {
    console.log(`\n[bus ] ${sig}, cerrando`);
    server.stop();
    disconnect();
    process.exit(0);
  });
}
