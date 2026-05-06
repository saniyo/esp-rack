// Stable per-tab client identifier. Generated once at first SPA load and
// persisted in sessionStorage so the same UUID survives feature-page
// navigation, REST retries and WebSocket reconnects within the tab. Cleared
// when the tab is closed (by sessionStorage semantics) — every new tab gets
// a fresh identity which matches how the admin Sockets view expects "one
// row per open tab".
const STORAGE_KEY = 'esp-react-client-key';

function generateKey(): string {
  if (typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function') {
    return crypto.randomUUID();
  }
  // Fallback for older browsers / non-secure contexts: timestamp + random.
  return (
    Date.now().toString(36) +
    '-' +
    Math.random().toString(36).slice(2) +
    Math.random().toString(36).slice(2)
  ).slice(0, 36);
}

let cachedKey: string | null = null;

export function getClientKey(): string {
  if (cachedKey) return cachedKey;
  try {
    const existing = sessionStorage.getItem(STORAGE_KEY);
    if (existing) {
      cachedKey = existing;
      return existing;
    }
    const fresh = generateKey();
    sessionStorage.setItem(STORAGE_KEY, fresh);
    cachedKey = fresh;
    return fresh;
  } catch {
    // sessionStorage can be unavailable in sandboxed iframes / private mode.
    // Fall back to an in-memory key so at least the current page lifetime
    // gets consistent identity.
    if (!cachedKey) cachedKey = generateKey();
    return cachedKey;
  }
}
