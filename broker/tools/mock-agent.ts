/**
 * Genera eventos de hook falsos contra el broker, para desarrollar la UI sin
 * depender de tener agentes de Claude Code trabajando de verdad.
 *
 *   bun run tools/mock-agent.ts            # 3 agentes, indefinidamente
 *   bun run tools/mock-agent.ts --agents 4 --speed 2
 */

const args = process.argv.slice(2);
const argOf = (name: string, def: number) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] ? Number(args[i + 1]) : def;
};

const N     = argOf("agents", 3);
const SPEED = argOf("speed", 1);          // >1 = mas rapido
const BASE  = process.env.BUS_URL ?? "http://127.0.0.1:8787";
const TOKEN = process.env.AGENT_BUS_TOKEN;

const REPOS = ["brain/api", "claude-agent-display", "civitatis-web", "sherpa", "nomad-health"];

const TOOLS: Array<{ name: string; input: () => Record<string, unknown>; ms: [number, number] }> = [
  { name: "Bash",      input: () => ({ command: pick(["pytest tests/ -x", "bun run build", "git status --short", "docker compose up -d", "npm run lint -- --fix", "git push --force origin main"]) }), ms: [1500, 9000] },
  { name: "Read",      input: () => ({ file_path: `/repo/src/${pick(["index.ts", "state.ts", "auth/middleware.py", "components/Card.tsx"])}` }), ms: [300, 1200] },
  { name: "Edit",      input: () => ({ file_path: `/repo/src/${pick(["main.cpp", "api/routes.ts", "models/user.py"])}` }), ms: [600, 2500] },
  { name: "Write",     input: () => ({ file_path: `/repo/${pick(["README.md", "docs/plan.md"])}` }), ms: [400, 1500] },
  { name: "Grep",      input: () => ({ pattern: pick(["TODO", "publishAgentState", "def handle_"]) }), ms: [300, 900] },
  { name: "WebSearch", input: () => ({ query: pick(["esp32 lvgl partial buffer", "mqtt retained qos1"]) }), ms: [1500, 4000] },
];

const ERRORS = ["rate_limit", "overloaded", "server_error"];

function pick<T>(xs: readonly T[]): T { return xs[Math.floor(Math.random() * xs.length)]!; }
function rnd(a: number, b: number) { return Math.floor(a + Math.random() * (b - a)); }
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms / SPEED));

async function post(path: string, payload: Record<string, unknown>) {
  try {
    const res = await fetch(`${BASE}${path}`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        ...(TOKEN ? { "X-Bus-Token": TOKEN } : {}),
      },
      body: JSON.stringify(payload),
    });
    if (!res.ok) console.error(`[mock] ${path} -> ${res.status}`);
  } catch (e) {
    console.error(`[mock] ${path} no responde:`, (e as Error).message);
  }
}

async function runAgent(i: number) {
  const session_id = crypto.randomUUID();
  const cwd = `/Users/andres/Projects/${REPOS[i % REPOS.length]}`;
  const base = { session_id, cwd, transcript_path: `/tmp/${session_id}.json`, permission_mode: "default" };

  await sleep(rnd(0, 2000));
  await post("/hook/session-start", { ...base, hook_event_name: "SessionStart", source: "startup" });
  console.log(`[mock] agente ${i} arrancado en ${cwd}`);

  for (;;) {
    // Rafaga de herramientas, luego descanso: se parece a una sesion real.
    const burst = rnd(2, 7);
    for (let k = 0; k < burst; k++) {
      const t = pick(TOOLS);
      await post("/hook/pre-tool", {
        ...base,
        hook_event_name: "PreToolUse",
        tool_name: t.name,
        tool_input: t.input(),
        tool_use_id: crypto.randomUUID(),
      });
      await sleep(rnd(t.ms[0], t.ms[1]));
    }

    if (Math.random() < 0.12) {
      const error_type = pick(ERRORS);
      await post("/hook/stop-failure", { ...base, hook_event_name: "StopFailure", error_type });
      console.log(`[mock] agente ${i} fallo: ${error_type}`);
      await sleep(rnd(4000, 10000));
    }

    await post("/hook/stop", { ...base, hook_event_name: "Stop", last_assistant_message: "listo" });
    await sleep(rnd(3000, 12000));          // idle, esperando al humano
  }
}

console.log(`[mock] ${N} agentes contra ${BASE} (velocidad x${SPEED}). Ctrl+C para parar.`);
for (let i = 0; i < N; i++) void runAgent(i);

process.on("SIGINT", () => { console.log("\n[mock] fin"); process.exit(0); });
