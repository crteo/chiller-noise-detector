import {mkdir,copyFile} from "node:fs/promises";
await mkdir("dist",{recursive:true});
for (const f of ["index.html","logo.png","sensor.js","firebase-client.js","firebase-config.js","measurement.js","demo.js"])
  await copyFile(f,`dist/${f}`);
console.log("Static site generated in dist/ (no firmware or credentials included).");
