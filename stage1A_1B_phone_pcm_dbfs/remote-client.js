const RECONNECT_DELAYS_MS = [1000, 2000, 4000, 8000, 15000];

function websocketUrl() {
  const protocol = location.protocol === "https:" ? "wss:" : "ws:";
  return `${protocol}//${location.host}/ws`;
}

export function createRemoteConnection({
  role,
  sessionId,
  token,
  onStatus,
  onMeasurement,
  onSensorStatus
}) {
  let socket = null;
  let stopped = false;
  let reconnectAttempt = 0;
  let reconnectTimer = null;
  let latestPendingMeasurement = null;
  let lastPublishedAt = 0;
  let publishTimer = null;

  function report(state, detail = "") {
    onStatus?.({ state, detail });
  }

  function send(message) {
    if (socket?.readyState !== WebSocket.OPEN) return false;
    socket.send(JSON.stringify(message));
    return true;
  }

  function flushMeasurement() {
    if (role !== "publisher" || !latestPendingMeasurement) return;

    const elapsed = performance.now() - lastPublishedAt;
    if (elapsed < 100) {
      if (!publishTimer) {
        publishTimer = setTimeout(() => {
          publishTimer = null;
          flushMeasurement();
        }, 100 - elapsed);
      }
      return;
    }

    if (send(latestPendingMeasurement)) {
      latestPendingMeasurement = null;
      lastPublishedAt = performance.now();
    }
  }

  function scheduleReconnect() {
    if (stopped || reconnectTimer) return;

    const delay = RECONNECT_DELAYS_MS[
      Math.min(reconnectAttempt, RECONNECT_DELAYS_MS.length - 1)
    ];
    reconnectAttempt += 1;
    report("reconnecting", `Retrying in ${delay / 1000}s`);

    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      connect();
    }, delay);
  }

  function connect() {
    if (stopped) return;

    report("connecting");
    socket = new WebSocket(websocketUrl());

    socket.addEventListener("open", () => {
      send({
        type: "hello",
        version: 1,
        role,
        sessionId,
        token
      });
    });

    socket.addEventListener("message", (event) => {
      let message;
      try {
        message = JSON.parse(event.data);
      } catch {
        return;
      }

      if (message.type === "ready") {
        reconnectAttempt = 0;
        report("connected");
        flushMeasurement();
      } else if (message.type === "measurement" && role === "subscriber") {
        onMeasurement?.(message);
      } else if (message.type === "sensor-status" && role === "subscriber") {
        onSensorStatus?.(message);
      } else if (message.type === "error") {
        report("error", message.message || "Relay error");
      }
    });

    socket.addEventListener("close", () => {
      socket = null;
      report("disconnected");
      scheduleReconnect();
    });

    socket.addEventListener("error", () => {
      report("error", "WebSocket connection failed");
    });
  }

  connect();

  return {
    publish(measurement) {
      if (role !== "publisher") return;
      latestPendingMeasurement = measurement;
      flushMeasurement();
    },

    close() {
      stopped = true;
      if (reconnectTimer) clearTimeout(reconnectTimer);
      if (publishTimer) clearTimeout(publishTimer);
      reconnectTimer = null;
      publishTimer = null;
      socket?.close();
      socket = null;
    }
  };
}

export function createSessionCredentials() {
  const bytes = crypto.getRandomValues(new Uint8Array(18));
  const token = Array.from(bytes, byte =>
    byte.toString(16).padStart(2, "0")
  ).join("");

  return {
    sessionId: token.slice(0, 8).toUpperCase(),
    token
  };
}
