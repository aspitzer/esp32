import mqtt, { type MqttClient } from "mqtt";
import type { AgentState, ActionRequest, ActionResponse, PermRequest, PermResponse } from "./types.ts";

const URL  = process.env.MQTT_URL  ?? "mqtt://127.0.0.1:1883";
const USER = process.env.MQTT_USER;
const PASS = process.env.MQTT_PASS;

let client: MqttClient | null = null;

/** Dispositivos vistos por su LWT: deviceId -> online */
const devices = new Map<string, boolean>();

/** agentIds con estado retained en el broker al arrancar este proceso. */
const staleRetained = new Set<string>();

type PermResponseHandler = (res: PermResponse) => void;
const permHandlers: PermResponseHandler[] = [];

export function onPermResponse(fn: PermResponseHandler) {
  permHandlers.push(fn);
}

type ActionHandler = (req: ActionRequest) => void;
const actionHandlers: ActionHandler[] = [];

export function onActionRequest(fn: ActionHandler) {
  actionHandlers.push(fn);
}

export function publishActionResult(res: ActionResponse) {
  client?.publish("claude/action/res", JSON.stringify(res), { qos: 1, retain: false });
}

/** ¿Hay al menos una placa viva? El broker solo enruta permisos si la hay. */
export function anyDeviceOnline(): boolean {
  for (const up of devices.values()) if (up) return true;
  return false;
}

export function deviceList(): Array<[string, boolean]> {
  return [...devices.entries()];
}

export function connect(): Promise<void> {
  return new Promise((resolve) => {
    client = mqtt.connect(URL, {
      username: USER,
      password: PASS,
      clientId: `agent-bus-${Math.random().toString(16).slice(2, 10)}`,
      clean: true,
      reconnectPeriod: 2000,
      connectTimeout: 10_000,
    });

    client.on("connect", () => {
      console.log(`[mqtt] conectado a ${URL}`);
      client!.subscribe(
        ["claude/perm/res", "claude/action/req", "claude/device/+/online", "claude/agents/+/state"],
        { qos: 1 },
      );
      resolve();
    });

    client.on("reconnect", () => console.log("[mqtt] reconectando..."));
    client.on("error", (e) => console.error("[mqtt] error:", e.message));
    client.on("close", () => console.log("[mqtt] conexion cerrada"));

    client.on("message", (topic, payload, packet) => {
      const ag = topic.match(/^claude\/agents\/([^/]+)\/state$/);
      if (ag?.[1]) {
        // Solo interesa el retained que ya estaba en el broker al arrancar:
        // son fantasmas de un proceso anterior que murio sin SessionEnd.
        if (packet.retain && payload.length > 0) staleRetained.add(ag[1]);
        return;
      }

      if (topic === "claude/perm/res") {
        try {
          const res = JSON.parse(payload.toString()) as PermResponse;
          for (const fn of permHandlers) fn(res);
        } catch {
          console.error("[mqtt] perm/res con JSON invalido:", payload.toString());
        }
        return;
      }

      if (topic === "claude/action/req") {
        try {
          const req = JSON.parse(payload.toString()) as ActionRequest;
          for (const fn of actionHandlers) fn(req);
        } catch {
          console.error("[mqtt] action/req con JSON invalido:", payload.toString());
        }
        return;
      }

      const m = topic.match(/^claude\/device\/([^/]+)\/online$/);
      if (m?.[1]) {
        const up = payload.toString() === "1";
        const prev = devices.get(m[1]);
        devices.set(m[1], up);
        if (prev !== up) console.log(`[mqtt] dispositivo ${m[1]} -> ${up ? "online" : "offline"}`);
      }
    });
  });
}

/** Estado de agente: SIEMPRE retained, para que la placa repinte tras un reinicio. */
export function publishAgentState(s: AgentState) {
  client?.publish(`claude/agents/${s.id}/state`, JSON.stringify(s), { qos: 1, retain: true });
}

/** Borra el retained de un agente (payload vacio). */
export function clearAgentState(agentId: string) {
  client?.publish(`claude/agents/${agentId}/state`, "", { qos: 1, retain: true });
}

export function publishPermRequest(req: PermRequest) {
  client?.publish("claude/perm/req", JSON.stringify(req), { qos: 1, retain: false });
}

/**
 * Un reinicio del broker significa que ninguna de las sesiones anteriores sigue
 * viva. Sus mensajes retained se quedarian en Mosquitto para siempre y la placa
 * los pintaria como agentes trabajando que ya no existen. Se borran al arrancar.
 *
 * Hay que esperar un poco: Mosquitto entrega los retained justo despues del
 * SUBACK, no de forma sincrona.
 */
export async function purgeStaleAgents(waitMs = 1500): Promise<number> {
  await new Promise((r) => setTimeout(r, waitMs));
  const n = staleRetained.size;
  for (const id of staleRetained) {
    clearAgentState(id);
    console.log(`[mqtt] retained fantasma borrado: ${id}`);
  }
  staleRetained.clear();
  return n;
}

export function disconnect() {
  client?.end();
}
