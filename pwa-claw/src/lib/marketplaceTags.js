const tagKey = (tag) => tag.trim().toLocaleLowerCase();

/** Return trimmed, case-insensitively unique display tags for one skill. */
export function skillTags(skill) {
  if (!Array.isArray(skill?.tags)) return [];
  const seen = new Set();
  const tags = [];
  for (const value of skill.tags) {
    if (typeof value !== "string" || !value.trim()) continue;
    const key = tagKey(value);
    if (seen.has(key)) continue;
    seen.add(key);
    tags.push(value.trim());
  }
  return tags;
}

/**
 * Validate gateway facets without changing their promoted-first order.
 * A malformed response returns null so the Store can use its local fallback.
 */
export function normalizeTagFacets(payload) {
  if (!Array.isArray(payload?.tags)) return null;
  const seen = new Set();
  const facets = [];
  for (const row of payload.tags) {
    if (typeof row?.tag !== "string" || !row.tag.trim()) continue;
    const key = tagKey(row.tag);
    if (seen.has(key)) continue;
    seen.add(key);
    const promoted = row.promoted === true;
    facets.push({
      tag: row.tag.trim(),
      count: Number.isFinite(row.count) ? Math.max(0, Math.trunc(row.count)) : 0,
      promoted,
      position: promoted && Number.isInteger(row.position) ? row.position : null,
    });
  }
  return facets;
}

/** Offline/degraded fallback: counts for the supplied skills, unpromoted. */
export function deriveTagFacets(skills) {
  const facets = new Map();
  if (!Array.isArray(skills)) return [];
  for (const skill of skills) {
    for (const tag of skillTags(skill)) {
      const key = tagKey(tag);
      const current = facets.get(key);
      facets.set(key, {
        tag: current?.tag || tag,
        count: (current?.count || 0) + 1,
        promoted: false,
        position: null,
      });
    }
  }
  return [...facets.values()].sort(
    (a, b) => b.count - a.count || a.tag.localeCompare(b.tag, undefined, { sensitivity: "base" }),
  );
}

/**
 * Limit gateway facets to tags present in the supplied skill category.
 * Preserve gateway promotion/order, but show counts for that category only.
 */
export function tagFacetsForSkills(gatewayFacets, skills) {
  const localFacets = deriveTagFacets(skills);
  if (!Array.isArray(gatewayFacets)) return localFacets;
  const localByTag = new Map(localFacets.map((facet) => [tagKey(facet.tag), facet]));
  return gatewayFacets.flatMap((facet) => {
    const local = localByTag.get(tagKey(facet.tag));
    return local ? [{ ...facet, count: local.count }] : [];
  });
}

export function skillHasTag(skill, tag) {
  if (typeof tag !== "string" || !tag.trim()) return true;
  const key = tagKey(tag);
  return skillTags(skill).some((value) => tagKey(value) === key);
}
