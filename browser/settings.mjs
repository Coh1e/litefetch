export const SETTINGS_KEY = 'handoff.settings.v1';

// One storage key per handoff, scoped by browser profile. The adapters write
// these keys and the popup lists them, so the format lives here: a copy in each
// file let the popup filter a retired prefix and show no recent takeovers.
export const JOURNAL_PREFIX = 'handoff.v1.';
export function journalKey(scope, id) {
  if (!['regular', 'private'].includes(scope)) throw new Error('Invalid journal scope');
  return `${JOURNAL_PREFIX}${scope}.${id}`;
}
export function isJournalKey(name) {
  return typeof name === 'string' && name.startsWith(JOURNAL_PREFIX);
}

// One switch. The old version also demanded a list of allowed sites, which turned
// the simplest possible request — "take this download" — into a configuration
// chore. Extra sites can still be excluded through `origins`, but nothing forces
// the user to fill it in.
export function normalizeSettings(value) {
  if (!value || typeof value !== 'object') return {enabled: false, origins: []};
  const list = Array.isArray(value.origins) ? value.origins : [];
  if (list.length > 100) return {enabled: false, origins: []};
  const origins = [];
  for (const text of list) {
    if (typeof text !== 'string') continue;
    try {
      const url = new URL(text);
      if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password
        || url.pathname !== '/' || url.search || url.hash) continue;
      origins.push(url.origin);
    } catch { continue; }
  }
  return {enabled: value.enabled === true, origins: [...new Set(origins)]};
}

// An empty list means every ordinary http(s) download; a non-empty one limits
// takeover to those sites.
export function allowsOrigin(settings, origin) {
  return settings.enabled && (settings.origins.length === 0 || settings.origins.includes(origin));
}
