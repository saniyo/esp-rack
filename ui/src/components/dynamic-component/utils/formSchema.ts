// SES-LIB-033: a static-schema feature's form = its schema (cached by the
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
