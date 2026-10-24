// [host]: schema + state = the form.
import { describe, expect, it } from '@jest/globals';

import { mergeSchemaState, staticStatePath } from './formSchema';

describe('static schema', () => {
  it('fills the values from the state', () => {
    const schema = {
      settings: {
        description: 'Meter',
        fields: [
          { t: 'cols', n: 2 },
          { lamp_power_w: null, o: 'number;rw;mn=0;mx=150' },
          { enabled: null, o: 'switch;rw' },
          { log: null, o: 'table;r;cols=ts:Time' },
          { a_reset: 'Reset', o: 'action;rw;ref=meter.reset' },
          { missing: null, o: 'text;r' },
        ],
      },
      other: 1,
    };
    const state = { lamp_power_w: 0, enabled: false, log: [{ ts: 1 }], extra: 5 };
    const m = mergeSchemaState(schema, state);
    const f = m.settings.fields;
    expect(f[0]).toEqual({ t: 'cols', n: 2 });
    expect(f[1]).toEqual({ lamp_power_w: 0, o: 'number;rw;mn=0;mx=150' });
    expect(Object.keys(f[1])[0]).toBe('lamp_power_w');  // the parser takes the first key
    expect(f[2].enabled).toBe(false);
    expect(f[3].log).toEqual([{ ts: 1 }]);
    expect(f[4].a_reset).toBe('Reset');  // a label, not data
    expect(f[5].missing).toBeNull();
    expect(m.settings.description).toBe('Meter');
    expect(m.other).toBe(1);
  });
});

describe('where a static-schema tab reads its values', () => {
  it('a static-schema feature: its own form tabs read its /state', () => {
    const e = { id: 'telegram', ss: 1 };
    expect(staticStatePath(e, { restPath: '/api/v1/telegram/form' })).toBe('/api/v1/telegram/state');
    expect(staticStatePath(e, { restPath: '/api/v1/other/form' })).toBeUndefined();  // contributed
  });
  it('a bespoke page tab names its state path', () => {
    const e = { id: 'system' };
    const hw = { restPath: '/api/v1/system/hardware/form', statePath: '/api/v1/system/hardware/state' };
    expect(staticStatePath(e, hw)).toBe('/api/v1/system/hardware/state');
    expect(staticStatePath(e, { restPath: '/api/v1/system/status/form' })).toBeUndefined();
  });
  it('anything else reads its full form', () => {
    expect(staticStatePath({ id: 'wifi' }, { restPath: '/api/v1/wifi/form' })).toBeUndefined();
  });
});
