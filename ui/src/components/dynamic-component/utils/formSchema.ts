// a static-schema feature's form = its schema (cached by the
// browser, ETag/304) + its flat /state. Each field takes its value from the
// state under the field's key; layout markers and labels stay as sent.

export function mergeSchemaState(form: Record<string, any>, state: Record<string, any>): Record<string, any> {
  const out: Record<string, any> = {};
  Object.entries(form ?? {}).forEach(([name, f]) => {
    if (!f || typeof f !== 'object' || !Array.isArray((f as any).fields)) {
      out[name] = f;
      return;
    }
    out[name] = {
      ...(f as any),
      fields: (f as any).fields.map((fld: any) => {
        if (!fld || typeof fld !== 'object' || !('o' in fld)) return fld;  // a layout marker
        const key = Object.keys(fld).find((k) => k !== 'o');
        if (!key || !state || !(key in state)) return fld;
        return { ...fld, [key]: state[key] };  // the key stays first
      }),
    };
  });
  return out;
}

// Where a static-schema tab reads its values, or undefined for
// a tab that reads its full form: a bespoke page tab names its state path; a
// static-schema feature's own tabs read /api/v1/<id>/state.
export function staticStatePath(
  entry: { id: string; ss?: number },
  tab: { restPath: string; statePath?: string },
): string | undefined {
  if (tab.statePath) return tab.statePath;
  if (entry.ss && tab.restPath === `/api/v1/${entry.id}/form`) return `/api/v1/${entry.id}/state`;
  return undefined;
}
