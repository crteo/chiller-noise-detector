// Bounded SSE decoder; exported for transport tests.
export function createSseParser(onEvent) {
  let buffer = "";
  return chunk => {
    buffer += chunk;
    if (buffer.length > 131072) throw new Error("Firebase event too large");
    let match;
    while ((match = /\r?\n\r?\n/.exec(buffer))) {
      const frame = buffer.slice(0, match.index);
      buffer = buffer.slice(match.index + match[0].length);
      let type = "message";
      const data = [];
      for (const line of frame.split(/\r?\n/)) {
        if (line.startsWith("event:")) type = line.slice(6).trim();
        if (line.startsWith("data:")) data.push(line.slice(5).trimStart());
      }
      if (data.length) onEvent(type, JSON.parse(data.join("\n")));
    }
  };
}
export function applyFirebaseEvent(snapshot, type, event) {
  if (!["put", "patch"].includes(type)) return snapshot;
  if (!event || typeof event.path !== "string" || !event.path.startsWith("/")) throw new Error("Invalid Firebase event");
  const root = structuredClone(snapshot ?? {});
  function set(path, value) {
    const keys = path.split("/").filter(Boolean);
    if (keys.some(k => ["__proto__", "constructor", "prototype"].includes(k))) throw new Error("Invalid Firebase path");
    if (!keys.length) return value;
    let target = root;
    for (const key of keys.slice(0, -1)) {
      if (!target[key] || typeof target[key] !== "object") target[key] = {};
      target = target[key];
    }
    if (value === null) delete target[keys.at(-1)];
    else target[keys.at(-1)] = value;
    return root;
  }
  if (type === "put") return set(event.path, event.data);
  if (!event.data || typeof event.data !== "object") throw new Error("Invalid Firebase patch");
  for (const [key, value] of Object.entries(event.data)) set(`${event.path}/${key}`, value);
  return root;
}
export function createFirebaseConnection({config, onStatus, onMeasurement, fetchImpl = fetch}) {
  let stopped = false, controller, retryTimer, expiryTimer, watchdog;
  let token = "", refreshToken = "", expiresAt = 0, attempt = 0;
  let snapshot = null;
  const authStorageKey = `noise-monitor-firebase-auth:${config.apiKey}`;
  try {
    const saved = JSON.parse(globalThis.localStorage?.getItem(authStorageKey));
    if (typeof saved?.refreshToken === "string") refreshToken = saved.refreshToken;
  } catch { /* Private browsing or disabled storage: use an in-memory session. */ }
  const report = (state, detail = "") => onStatus?.({state, detail});
  async function authenticate(signal) {
    if (token && Date.now() < expiresAt) return;
    const refreshing = Boolean(refreshToken);
    const url = refreshing
      ? `https://securetoken.googleapis.com/v1/token?key=${encodeURIComponent(config.apiKey)}`
      : `https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=${encodeURIComponent(config.apiKey)}`;
    const response = await fetchImpl(url, {
      method:"POST", signal,
      headers:{"Content-Type": refreshing ? "application/x-www-form-urlencoded" : "application/json"},
      body: refreshing ? new URLSearchParams({grant_type:"refresh_token", refresh_token:refreshToken})
        : JSON.stringify({returnSecureToken:true})
    });
    if (!response.ok) {
      token = ""; refreshToken = "";
      try { globalThis.localStorage?.removeItem(authStorageKey); } catch {}
      throw new Error(`Firebase authentication failed (${response.status}); check API key and Anonymous sign-in`);
    }
    const data = await response.json();
    token = data.idToken || data.id_token;
    refreshToken = data.refreshToken || data.refresh_token;
    if (!token || !refreshToken) throw new Error("Invalid authentication response");
    expiresAt = Date.now() + (Number(data.expiresIn || data.expires_in) - 60) * 1000;
    try { globalThis.localStorage?.setItem(authStorageKey, JSON.stringify({refreshToken})); } catch {}
  }
  function touch() {
    clearTimeout(watchdog);
    watchdog = setTimeout(() => controller?.abort(), 45000);
  }
  async function connect() {
    if (stopped) return;
    controller = new AbortController();
    report("connecting");
    touch();
    try {
      await authenticate(controller.signal);
      const url = new URL(config.databaseURL);
      url.searchParams.set("auth", token);
      const response = await fetchImpl(url, {headers:{Accept:"text/event-stream"}, cache:"no-store", signal:controller.signal});
      if (!response.ok || !response.body) {
        if (response.status === 401) expiresAt = 0;
        throw new Error(`Firebase read failed (${response.status}); check database rules`);
      }
      report("connected");
      snapshot = null;
      expiryTimer = setTimeout(() => controller.abort(), Math.max(1000, expiresAt - Date.now()));
      const parser = createSseParser((type, event) => {
        touch();
        if (type === "cancel") throw new Error("Firebase access cancelled; check database rules");
        if (type === "auth_revoked") { expiresAt = 0; throw new Error("Firebase authentication expired"); }
        if (type === "put" || type === "patch") {
          snapshot = applyFirebaseEvent(snapshot, type, event);
          onMeasurement(snapshot);
          attempt = 0;
        }
      });
      const reader = response.body.getReader();
      const decoder = new TextDecoder();
      try {
        while (!stopped) {
          const {done, value} = await reader.read();
          if (done) break;
          touch();
          parser(decoder.decode(value, {stream:true}));
        }
      } finally { await reader.cancel().catch(() => {}); reader.releaseLock(); }
      if (!stopped) throw new Error("Firebase stream closed");
    } catch (error) {
      if (!stopped) report("reconnecting", error.name === "AbortError" ? "Refreshing Firebase connection" : error.message);
    } finally {
      controller.abort();
      clearTimeout(expiryTimer); clearTimeout(watchdog);
      if (!stopped) retryTimer = setTimeout(connect, Math.min(15000, 1000 * 2 ** Math.min(attempt++, 4)));
    }
  }
  if (!config.apiKey) report("error", "Set apiKey in firebase-config.js; see deployment.md");
  else connect();
  return {close() {
    stopped = true;
    controller?.abort();
    clearTimeout(retryTimer); clearTimeout(expiryTimer); clearTimeout(watchdog);
    report("paused");
  }};
}
