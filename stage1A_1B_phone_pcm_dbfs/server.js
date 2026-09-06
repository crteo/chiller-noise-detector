import { createServer } from "node:http";
import { readFile, stat } from "node:fs/promises";
import { extname, join, normalize } from "node:path";
import { fileURLToPath } from "node:url";
import { WebSocketServer, WebSocket } from "ws";

const root = fileURLToPath(new URL(".", import.meta.url));
const port = Number(process.env.PORT) || 8080;
const sessions = new Map();

const mimeTypes = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".md": "text/markdown; charset=utf-8"
};

const server = createServer(async (request, response) => {
  try {
    const url = new URL(request.url, `http://${request.headers.host}`);
    const requestedPath = url.pathname === "/" ? "/index.html" : url.pathname;
    const safePath = normalize(requestedPath).replace(/^(\.\.(\/|\\|$))+/, "");
    const filePath = join(root, safePath);

    if (!filePath.startsWith(root)) throw new Error("Invalid path");
    const info = await stat(filePath);
    if (!info.isFile()) throw new Error("Not a file");

    response.writeHead(200, {
      "Content-Type": mimeTypes[extname(filePath)] || "application/octet-stream",
      "Cache-Control": "no-cache"
    });
    response.end(await readFile(filePath));
  } catch {
    response.writeHead(404, { "Content-Type": "text/plain; charset=utf-8" });
    response.end("Not found");
  }
});

const webSocketServer = new WebSocketServer({ server, path: "/ws" });

function send(socket, message) {
  if (socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify(message));
  }
}

function removeClient(socket) {
  const membership = socket.membership;
  if (!membership) return;

  const session = sessions.get(membership.sessionId);
  if (!session) return;

  if (membership.role === "publisher" && session.publisher === socket) {
    session.publisher = null;
    for (const subscriber of session.subscribers) {
      send(subscriber, { type: "sensor-status", state: "disconnected" });
    }
  } else {
    session.subscribers.delete(socket);
  }

  if (!session.publisher && session.subscribers.size === 0) {
    sessions.delete(membership.sessionId);
  }
}

webSocketServer.on("connection", socket => {
  socket.isAlive = true;
  socket.on("pong", () => { socket.isAlive = true; });

  socket.on("message", raw => {
    if (raw.length > 16384) {
      socket.close(1009, "Message too large");
      return;
    }

    let message;
    try {
      message = JSON.parse(raw.toString());
    } catch {
      send(socket, { type: "error", message: "Invalid JSON" });
      return;
    }

    if (!socket.membership) {
      if (
        message.type !== "hello" ||
        !["publisher", "subscriber"].includes(message.role) ||
        !/^[A-Z0-9]{8}$/.test(message.sessionId || "") ||
        !/^[a-f0-9]{36}$/.test(message.token || "")
      ) {
        socket.close(1008, "Invalid session credentials");
        return;
      }

      let session = sessions.get(message.sessionId);
      if (!session) {
        session = {
          token: message.token,
          publisher: null,
          subscribers: new Set(),
          latestMeasurement: null
        };
        sessions.set(message.sessionId, session);
      }

      if (session.token !== message.token) {
        socket.close(1008, "Unauthorized");
        return;
      }

      if (message.role === "publisher") {
        if (session.publisher && session.publisher !== socket) {
          socket.close(1008, "Publisher already connected");
          return;
        }
        session.publisher = socket;
      } else {
        session.subscribers.add(socket);
      }

      socket.membership = {
        role: message.role,
        sessionId: message.sessionId
      };
      send(socket, { type: "ready", role: message.role });

      if (message.role === "subscriber" && session.latestMeasurement) {
        send(socket, session.latestMeasurement);
      }
      if (message.role === "subscriber" && !session.publisher) {
        send(socket, { type: "sensor-status", state: "disconnected" });
      }
      return;
    }

    if (
      socket.membership.role === "publisher" &&
      message.type === "measurement" &&
      message.version === 1 &&
      Number.isFinite(message.measuredAt) &&
      message.values &&
      JSON.stringify(message).length <= 8192
    ) {
      const session = sessions.get(socket.membership.sessionId);
      const relayMessage = {
        ...message,
        relayedAt: Date.now()
      };
      session.latestMeasurement = relayMessage;
      for (const subscriber of session.subscribers) {
        send(subscriber, relayMessage);
      }
    }
  });

  socket.on("close", () => removeClient(socket));
});

const heartbeat = setInterval(() => {
  for (const socket of webSocketServer.clients) {
    if (!socket.isAlive) {
      socket.terminate();
      continue;
    }
    socket.isAlive = false;
    socket.ping();
  }
}, 30000);

webSocketServer.on("close", () => clearInterval(heartbeat));
server.listen(port, () => {
  console.log(`Dashboard and relay listening on http://localhost:${port}`);
});
