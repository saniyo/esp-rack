// Fallback only — at runtime the AppBar / sidebar / document.title
// all derive from `manifest.device.name` (which comes from
// ESPRack::Builder("ProjectName", ...) on the device). This constant
// is what gets shown for the ~100 ms between React mount and the
// first manifest fetch resolving, plus the rare case where the
// device returned a manifest with no `device.name`. "ESPRack" is
// the framework-neutral brand; consumer apps can override via the
// REACT_APP_PROJECT_NAME env var at build time if they want their
// own pre-manifest brand to show.
export const PROJECT_NAME = process.env.REACT_APP_PROJECT_NAME || 'ESPRack';
export const PROJECT_PATH = process.env.REACT_APP_PROJECT_PATH || 'project';
