import assert from "node:assert/strict";
import test from "node:test";
import { listSkillTags } from "../src/lib/marketplaceApi.js";

test("tag facets use the public marketplace proxy route", async (t) => {
  const payload = {
    tags: [{ tag: "dance", count: 4, promoted: true, position: 0 }],
    promoted_tags: ["dance"],
  };
  t.mock.method(globalThis, "fetch", async (url) => {
    assert.equal(url, "/v1/marketplace/skills/tags");
    return { ok: true, json: async () => payload };
  });
  assert.deepEqual(await listSkillTags(), payload);
});

test("tag facet HTTP failures allow the Store to use its fallback", async (t) => {
  t.mock.method(globalThis, "fetch", async () => ({ ok: false, status: 503 }));
  await assert.rejects(listSkillTags(), /Failed to list skill tags: 503/);
});
