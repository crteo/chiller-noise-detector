import {createServer} from "node:http";
import {readFile} from "node:fs/promises";
const assets = new Map([
  ["/", "index.html"], ["/index.html", "index.html"], ["/logo.png", "logo.png"],
  ...["sensor.js","firebase-client.js","firebase-config.js","measurement.js","demo.js"].map(f => [`/${f}`,f])
]);
export function createStaticServer() {
  return createServer(async (req,res) => {
    const path = new URL(req.url,"http://localhost").pathname;
    const file = assets.get(path);
    if (!file || !["GET","HEAD"].includes(req.method)) {res.writeHead(404); res.end("Not found"); return;}
    try {
      const data = await readFile(new URL(file,import.meta.url));
      res.writeHead(200, {"Content-Type":file.endsWith(".png") ? "image/png" : file.endsWith(".js") ? "text/javascript; charset=utf-8" : "text/html; charset=utf-8", "Cache-Control":"no-store", "X-Content-Type-Options":"nosniff"});
      res.end(req.method === "HEAD" ? undefined : data);
    } catch {res.writeHead(404);res.end("Not found");}
  });
}
if (process.argv[1] && import.meta.url === new URL(process.argv[1],"file:").href) {
  createStaticServer().listen(Number(process.env.PORT)||8080,"0.0.0.0",() => console.log("Dashboard: http://localhost:8080 (demo: /?demo=1)"));
}
