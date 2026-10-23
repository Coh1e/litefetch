export const SETTINGS_KEY = 'handoff.settings.v1';
export function normalizeSettings(value) {
  if (!value || typeof value !== 'object' || !Array.isArray(value.origins) || value.origins.length > 100) {
    return {enabled: false, origins: []};
  }
  const origins = [];
  for (const text of value.origins) {
    if (typeof text !== 'string') return {enabled: false, origins: []};
    try {
      const url = new URL(text);
      if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password
        || url.pathname !== '/' || url.search || url.hash) return {enabled: false, origins: []};
      origins.push(url.origin);
    } catch { return {enabled: false, origins: []}; }
  }
  return {enabled: value.enabled === true, origins: [...new Set(origins)]};
}
