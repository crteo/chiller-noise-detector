import test from "node:test";
import assert from "node:assert/strict";
import {applyFirebaseEvent, createSseParser} from "../firebase-client.js";

test("SSE parser handles frames split across chunks", () => {
  const received = [];
  const parse = createSseParser((type, data) => received.push({type, data}));
  parse("event: put\ndata: {\"path\":\"/\",\"da");
  parse("ta\":{\"version\":2}}\n\n");
  assert.deepEqual(received, [{type:"put", data:{path:"/", data:{version:2}}}]);
});

test("Firebase put and patch events update the snapshot", () => {
  let snapshot = applyFirebaseEvent(null, "put", {path:"/", data:{status:{capture:"running"}}});
  snapshot = applyFirebaseEvent(snapshot, "patch", {path:"/status", data:{clockSynced:true}});
  assert.deepEqual(snapshot, {status:{capture:"running", clockSynced:true}});
});

test("dangerous Firebase paths are rejected", () => {
  assert.throws(() => applyFirebaseEvent({}, "put", {path:"/__proto__/polluted", data:true}));
});

