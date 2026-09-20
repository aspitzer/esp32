/**
 * Dispara una peticion de permiso REAL contra el broker para poder probar el
 * modal de la placa y la confirmacion con BOOT.
 *
 * No ejecuta nada: solo recorre el camino del hook. El comando es texto que se
 * pinta en pantalla, nunca se lanza. Es la unica forma de probar la fase 2 sin
 * destruir algo de verdad, porque el hook PermissionRequest solo se dispara
 * cuando Claude Code iba a preguntar, y solo pregunta por lo destructivo.
 *
 *   bun run tools/test-permission.ts
 *   bun run tools/test-permission.ts "git push --force origin main"
 *   bun run tools/test-permission.ts "rm -rf ~/Proyectos" Bash
 */

const cmd   = process.argv[2] ?? "rm -rf ~/Projects/esp32 --no-preserve-root";
const tool  = process.argv[3] ?? "Bash";
const BASE  = process.env.BUS_URL ?? "http://127.0.0.1:8787";
const TOKEN = process.env.AGENT_BUS_TOKEN;

const headers = {
  "Content-Type": "application/json",
  ...(TOKEN ? { "X-Bus-Token": TOKEN } : {}),
};

const session_id = "00000000-0000-4000-8000-0000000000ff";   // fijo: siempre el mismo slot
const base = { session_id, cwd: "/tmp/PRUEBA", hook_event_name: "PermissionRequest" };

console.log(`\nSIMULACRO. Nada de esto se ejecuta.`);
console.log(`  herramienta : ${tool}`);
console.log(`  comando     : ${cmd}\n`);

await fetch(`${BASE}/hook/session-start`, {
  method: "POST", headers,
  body: JSON.stringify({ ...base, hook_event_name: "SessionStart" }),
});

const health = await (await fetch(`${BASE}/health`)).json() as {
  devices: Array<[string, boolean]>;
};
const online = health.devices.some(([, up]) => up);
console.log(online
  ? "Placa online. Mira la pantalla: toca DENEGAR o PERMITIR y confirma con BOOT."
  : "AVISO: ninguna placa online, esto va a caer al fallback en cuanto salga.");
console.log(`Sin respuesta, el broker corta a los ${Number(process.env.PERM_TIMEOUT_MS ?? 90000) / 1000} s.\n`);

const t0 = Date.now();
const res = await fetch(`${BASE}/hook/permission`, {
  method: "POST", headers,
  body: JSON.stringify({ ...base, tool_name: tool, tool_input: { command: cmd, file_path: cmd } }),
});
const body = await res.json() as {
  hookSpecificOutput?: { decision?: { verdict?: string; reason?: string } };
};
const d = body.hookSpecificOutput?.decision;
const secs = ((Date.now() - t0) / 1000).toFixed(1);

console.log(`Respuesta en ${secs} s: ${d?.verdict}`);
console.log(`  motivo: ${d?.reason}`);
console.log(d?.verdict === "allow"  ? "\nHabrias APROBADO ese comando."
          : d?.verdict === "deny"   ? "\nHabrias DENEGADO ese comando."
          : "\nSin decision: la sesion habria seguido por el flujo normal del portatil.");

await fetch(`${BASE}/hook/session-end`, {
  method: "POST", headers,
  body: JSON.stringify({ ...base, hook_event_name: "SessionEnd" }),
});

export {};
