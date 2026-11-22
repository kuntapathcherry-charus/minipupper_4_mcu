import assert from "node:assert/strict";
import test from "node:test";
import {
  deriveTagFacets,
  normalizeTagFacets,
  skillHasTag,
  skillTags,
  tagFacetsForSkills,
} from "../src/lib/marketplaceTags.js";

test("skill tags are trimmed and de-duplicated case-insensitively", () => {
  assert.deepEqual(skillTags({ tags: [" Dance ", "dance", "gaits", "", null, 2] }), ["Dance", "gaits"]);
  assert.deepEqual(skillTags({}), []);
  assert.equal(skillHasTag({ tags: ["Dance"] }, " dance "), true);
  assert.equal(skillHasTag({ tags: ["Dance"] }, "gaits"), false);
});

test("gateway facets retain promoted-first response order and catalogue counts", () => {
  const payload = {
    tags: [
      { tag: "gaits", count: 2, promoted: true, position: 1 },
      { tag: "Dance", count: 4, promoted: true, position: 0 },
      { tag: "tools", count: 9, promoted: false, position: null },
    ],
    promoted_tags: ["gaits", "Dance"],
  };
  assert.deepEqual(normalizeTagFacets(payload), payload.tags);
});

test("malformed facet responses request fallback and invalid rows are ignored", () => {
  assert.equal(normalizeTagFacets(null), null);
  assert.equal(normalizeTagFacets({}), null);
  assert.deepEqual(normalizeTagFacets({ tags: [
    { tag: " Dance ", count: 4, promoted: true, position: 0 },
    { tag: "dance", count: 99, promoted: false, position: null },
    { tag: "", count: 2 },
    { tag: 4, count: 2 },
    null,
  ] }), [
    { tag: "Dance", count: 4, promoted: true, position: 0 },
  ]);
});

test("category facets exclude other skill types and use category counts", () => {
  const gateway = [
    { tag: "Walk", count: 4, promoted: true, position: 1 },
    { tag: "Dance", count: 3, promoted: true, position: 0 },
    { tag: "Shopping", count: 9, promoted: false, position: null },
    { tag: "Web", count: 8, promoted: false, position: null },
  ];
  const webSkills = [
    { tags: ["web", "Shopping"] },
    { tags: ["Web", "web"] },
  ];
  assert.deepEqual(tagFacetsForSkills(gateway, webSkills), [
    { tag: "Shopping", count: 1, promoted: false, position: null },
    { tag: "Web", count: 2, promoted: false, position: null },
  ]);
  assert.deepEqual(
    tagFacetsForSkills(null, webSkills),
    deriveTagFacets(webSkills),
  );
});

test("fallback counts each listing once and sorts by count then name", () => {
  const facets = deriveTagFacets([
    { tags: ["tools", "Dance", "dance"] },
    { tags: ["dance", "Gaits"] },
    { tags: ["TOOLS", "gaits"] },
    { tags: ["tools"] },
    { tags: "not-an-array" },
  ]);
  assert.deepEqual(facets, [
    { tag: "tools", count: 3, promoted: false, position: null },
    { tag: "Dance", count: 2, promoted: false, position: null },
    { tag: "Gaits", count: 2, promoted: false, position: null },
  ]);
});
