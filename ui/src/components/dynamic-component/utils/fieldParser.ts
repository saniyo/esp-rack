// `showIf`, `card`, `cardTitle`, `cardDeletable`, `colorMap` are universal
// tags every field type can carry. showIf gates rendering; card* are picked
// up by DynamicContentHandler to wrap consecutive same-group fields in a
// MUI <Card>; colorMap drives live-color Avatar on read-only renders.
export const fieldConfigurations = {
  text: { options: ['r', 'rw', 'pl', 'icon', 'unit', 'color', 'colorMap', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const },
  // secret — same option vocabulary as text minus `unit` and the now-
  // removed `password` flag. Routed to SecretField on the React side
  // (type=password input + eye-toggle).
  secret: { options: ['r', 'rw', 'pl', 'icon', 'color', 'colorMap', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const },
  number: { options: ['r', 'rw', 'mn', 'mx', 'f', 'pl', 'icon', 'unit', 'color', 'colorMap', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const },
  slider: { options: ['r', 'rw', 'mn', 'mx', 'pl', 'st', 'icon', 'color', 'colorMap', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const },
  checkbox: { options: ['r', 'rw', 'pl', 'icon', 'color', 'colorMap', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const },
  button: { options: ['r', 'rw', 'pl', 'icon', 'color', 'colorMap', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const },
  switch: { options: ['r', 'rw', 'pl', 'icon', 'color', 'colorMap', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const },
  dropdown: { options: ['r', 'rw', 'options', 'pl', 'icon', 'color', 'colorMap', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const },
  textarea: { options: ['r', 'rw', 'pl', 'icon', 'color', 'colorMap', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const },
  radio: { options: ['r', 'rw', 'options', 'pl', 'icon', 'color', 'colorMap', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const },
  trend: { options: ['to', 'xAxis', 'lines', 'legend', 'tooltip', 'maxPoints', 'mode', 'icon', 'color', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const },
  files: { options: ['r', 'rw', 'base', 'initial', 'uploadBase', 'icon', 'color', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const },
  upload: { options: ['r', 'rw', 'url', 'accept', 'icon', 'color', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const },
  // table supports two render modes via layout:
  //   default (grid)    — MUI data grid with col|label|type|fmt headers
  //   layout("cards")   — avatar-list style, one card per row with
  //                       primary/secondary/avatar/badge rendering.
  //                       rowFill / rowNav dispatch on row click either way.
  table: { options: ['r', 'rw', 'cols', 'icon', 'color', 'mode', 'maxRows', 'rowFill', 'rowFillFallback', 'rowNav', 'layout', 'primary', 'secondary', 'avatar', 'badge', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const },
  datetime: { options: ['r', 'rw', 'dtmode', 'icon', 'color', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const },
  action: { options: ['r', 'rw', 'url', 'method', 'confirm', 'color', 'icon', 'ref', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const },
  message: { options: ['r', 'rw', 'level', 'icon', 'color', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const },
  timezones: { options: ['r', 'rw', 'icon', 'color', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const },
  ip: { options: ['r', 'rw', 'pl', 'icon', 'color', 'colorMap', 'showIf', 'card', 'cardTitle', 'cardDeletable', 'hideAvatar', 'refetchForm', 'loadingIf'] as const }
};

export type FieldType = keyof typeof fieldConfigurations;
export type FieldOptionKey = typeof fieldConfigurations[FieldType]['options'][number];

export interface ShowIfSpec {
  field: string;
  val: string;
}

export interface RowFillPair {
  target: string;
  col: string;
}

export interface BadgeSpec {
  field: string;
  unit: string;
  iconName: string;
}

export type ColorMap = Record<string, string>;

export interface ParsedFieldOption {
  key: FieldOptionKey;
  value:
    | string
    | boolean
    | string[]
    | number
    | ShowIfSpec
    | RowFillPair[]
    | BadgeSpec
    | ColorMap
    | undefined;
}

export interface ParsedFieldOptions {
  readableLabel: string;
  // True iff backend provided an explicit `l=…` label override. Lets
  // elements decide whether to expose a hover tooltip with the
  // underlying JSON key (no point when the label is just the auto
  // titlecase of the key — they'd be redundant).
  labelOverridden: boolean;
  optionMap: Partial<Record<FieldOptionKey, ParsedFieldOption>>;
  finalType: FieldType;
}

/**
 * Хелпер: повертає значення після 'key=' або після 'key' (обидва формати підтримуються).
 * Напр., getAfter('mn=10.00','mn') -> '10.00'; getAfter('plHint','pl') -> 'Hint'
 */
function getAfter(opt: string, key: string): string {
  if (opt.startsWith(key + '=')) return opt.slice((key + '=').length);
  if (opt.startsWith(key)) return opt.slice(key.length);
  return '';
}

/** Парсери опцій (виправлено з урахуванням '=') */
const optionParsers: Record<string, (option: string) => ParsedFieldOption> = {
  r: () => ({ key: 'r', value: true }),
  rw: () => ({ key: 'rw', value: false }),

  // ВИПРАВЛЕНО: було slice(2) → давало '=10.00' => NaN
  mn: (opt) => {
    const raw = getAfter(opt, 'mn');
    const n = parseFloat(raw);
    return { key: 'mn', value: Number.isFinite(n) ? n : undefined };
  },

  // ВИПРАВЛЕНО: було slice(2)
  mx: (opt) => {
    const raw = getAfter(opt, 'mx');
    const n = parseFloat(raw);
    return { key: 'mx', value: Number.isFinite(n) ? n : undefined };
  },

  // ВИПРАВЛЕНО: бек шле f=..., раніше було slice(1)
  f: (opt) => {
    const raw = getAfter(opt, 'f');
    const n = parseFloat(raw);
    return { key: 'f', value: Number.isFinite(n) ? n : undefined };
  },

  // ВИПРАВЛЕНО: бек шле pl=..., раніше було slice(2)
  pl: (opt) => {
    const val = getAfter(opt, 'pl').trim();
    return { key: 'pl', value: val };
  },

  // ДОДАНО: step для slider (і можна юзати з number)
  st: (opt) => {
    const raw = getAfter(opt, 'st');
    const n = parseFloat(raw);
    return { key: 'st', value: Number.isFinite(n) ? n : undefined };
  },

  to: (opt) => ({ key: 'to', value: getAfter(opt, 'to') }),
  legend: () => ({ key: 'legend', value: true }),
  tooltip: () => ({ key: 'tooltip', value: true }),

  // lines=key1:color=...,key2:color=...
  lines: (opt) => {
    const val = getAfter(opt, 'lines');
    return { key: 'lines', value: val };
  },

  // options=Option1,Option2
  options: (opt) => {
    const val = getAfter(opt, 'options');
    const items = val.split(',').map((s) => s.trim()).filter(Boolean);
    return { key: 'options', value: items };
  },

  maxPoints: (opt) => {
    const raw = getAfter(opt, 'maxPoints');
    const n = parseInt(raw, 10);
    return { key: 'maxPoints', value: Number.isFinite(n) ? n : undefined };
  },

  // Cap retained rows for append/upsert-mode tables (ignored in replace).
  maxRows: (opt) => {
    const raw = getAfter(opt, 'maxRows');
    const n = parseInt(raw, 10);
    return { key: 'maxRows', value: Number.isFinite(n) ? n : undefined };
  },

  mode: (opt) => {
    const val = getAfter(opt, 'mode').trim();
    return { key: 'mode', value: val };
  },

  base: (opt) => ({ key: 'base', value: getAfter(opt, 'base') }),
  initial: (opt) => ({ key: 'initial', value: getAfter(opt, 'initial') }),
  uploadBase: (opt) => ({ key: 'uploadBase', value: getAfter(opt, 'uploadBase') }),
  url: (opt) => ({ key: 'url', value: getAfter(opt, 'url') }),
  accept: (opt) => ({ key: 'accept', value: getAfter(opt, 'accept') }),

  // Columns descriptor for table:
  //   cols=col1|label=X|type=Y|fmt=Z,col2|label=A|type=B
  // The TableField component parses this further; parser keeps the raw string.
  cols: (opt) => ({ key: 'cols', value: getAfter(opt, 'cols') }),

  // Universal icon tag — name must match an entry in iconRegistry.ts whose
  // key equals the @mui/icons-material file name (e.g. icon=Thermostat).
  icon: (opt) => ({ key: 'icon', value: getAfter(opt, 'icon') }),

  // DateTimeField HTML input variant: date | time | datetime.
  dtmode: (opt) => ({ key: 'dtmode', value: getAfter(opt, 'dtmode') }),

  // (ActionField reuses the same `url` parser declared above for UploadField.)

  // ActionField HTTP method (defaults to POST when missing).
  method: (opt) => ({ key: 'method', value: getAfter(opt, 'method') }),

  // ActionField optional confirm dialog text.
  confirm: (opt) => ({ key: 'confirm', value: getAfter(opt, 'confirm') }),

  // ActionField MUI color variant: primary|secondary|error|warning|success|info|inherit.
  color: (opt) => ({ key: 'color', value: getAfter(opt, 'color') }),

  // MessageField MUI Alert severity.
  level: (opt) => ({ key: 'level', value: getAfter(opt, 'level') }),

  // ActionField reference to a WebManager-registered action id. When set,
  // the frontend pulls URL/method/confirm/color/icon from the manifest
  // instead of reading the inline url/method/confirm/color options.
  ref: (opt) => ({ key: 'ref', value: getAfter(opt, 'ref') }),

  // Display unit suffix (rendered after value in read-only views).
  unit: (opt) => ({ key: 'unit', value: getAfter(opt, 'unit') }),

  // Conditional visibility: "<field>:<value>" — field shows only when the
  // form's <field> stringifies to <value>. Stringified compare so a boolean
  // true matches showIf(...,"true").
  showIf: (opt) => {
    const raw = getAfter(opt, 'showIf');
    const colon = raw.indexOf(':');
    if (colon < 0) return { key: 'showIf', value: undefined };
    return {
      key: 'showIf',
      value: { field: raw.slice(0, colon).trim(), val: raw.slice(colon + 1).trim() }
    };
  },

  // Table row-click dispatch: "<target>=<col>,<target>=<col>,..."
  // Empty <col> → clear <target>.
  rowFill: (opt) => {
    const raw = getAfter(opt, 'rowFill');
    const pairs = raw
      .split(',')
      .map((s) => s.trim())
      .filter(Boolean)
      .map((pair) => {
        const eq = pair.indexOf('=');
        if (eq < 0) return null;
        return { target: pair.slice(0, eq).trim(), col: pair.slice(eq + 1).trim() };
      })
      .filter((x): x is RowFillPair => x !== null);
    return { key: 'rowFill', value: pairs };
  },

  // Smart-slot fallback for rowFill — same syntax. TableField inspects
  // the FIRST target's current value in formState; if that field is
  // empty → use rowFill (primary), otherwise → try rowFillFallback's
  // first target (secondary). Both filled → dialog asks the operator.
  rowFillFallback: (opt) => {
    const raw = getAfter(opt, 'rowFillFallback');
    const pairs = raw
      .split(',')
      .map((s) => s.trim())
      .filter(Boolean)
      .map((pair) => {
        const eq = pair.indexOf('=');
        if (eq < 0) return null;
        return { target: pair.slice(0, eq).trim(), col: pair.slice(eq + 1).trim() };
      })
      .filter((x): x is RowFillPair => x !== null);
    return { key: 'rowFillFallback', value: pairs };
  },

  // Router navigation target — paired with rowFill. On row click, TableField
  // navigates to this path carrying the filled values as location state.
  rowNav: (opt) => ({ key: 'rowNav', value: getAfter(opt, 'rowNav').trim() }),

  // table `layout=grid|cards` picks between data-grid and avatar-card list.
  layout: (opt) => ({ key: 'layout', value: getAfter(opt, 'layout').trim() }),

  // listCard row primitives — all consume raw field names off the data row.
  primary:   (opt) => ({ key: 'primary',   value: getAfter(opt, 'primary').trim() }),
  secondary: (opt) => ({ key: 'secondary', value: getAfter(opt, 'secondary') }),  // keep template verbatim (spaces, punctuation)
  avatar:    (opt) => ({ key: 'avatar',    value: getAfter(opt, 'avatar').trim() }),
  badge:     (opt) => {
    const raw = getAfter(opt, 'badge');
    const parts = raw.split('|');
    return {
      key: 'badge',
      value: {
        field: (parts[0] || '').trim(),
        unit: (parts[1] || '').trim(),
        iconName: (parts[2] || '').trim()
      } as BadgeSpec
    };
  },

  // value→color palette key mapping. "Connected:success,Connecting:warning,default:info"
  colorMap: (opt) => {
    const raw = getAfter(opt, 'colorMap');
    const map: ColorMap = {};
    raw.split(',').forEach((pair) => {
      const colon = pair.indexOf(':');
      if (colon < 0) return;
      const k = pair.slice(0, colon).trim();
      const v = pair.slice(colon + 1).trim();
      if (k) map[k] = v;
    });
    return { key: 'colorMap', value: map };
  },

  // Card group wrapper — grouping id; cardTitle/cardDeletable carried on first
  // field of the group. DynamicContentHandler consumes these to render a
  // single <Card> around a consecutive run of fields sharing the id.
  card:           (opt) => ({ key: 'card',           value: getAfter(opt, 'card').trim() }),
  cardTitle:      (opt) => ({ key: 'cardTitle',      value: getAfter(opt, 'cardTitle') }),
  cardDeletable:  (opt) => ({ key: 'cardDeletable',  value: getAfter(opt, 'cardDeletable') === '1' }),

  // Universal avatar/label suppressor — widgets that render a leading
  // Avatar (read-only text, table header, list-card empty state, …) skip
  // it when hideAvatar=1.
  hideAvatar:     (opt) => ({ key: 'hideAvatar',     value: getAfter(opt, 'hideAvatar') === '1' }),

  // Triggers a short burst of form refetches after the action's HTTP
  // call succeeds. Implemented in ActionField; uses the refetch hook
  // provided by DynamicFormContext.
  refetchForm:    (opt) => ({ key: 'refetchForm',    value: getAfter(opt, 'refetchForm') === '1' }),

  // Reuses the showIf parse shape: "<field>:<valuePrefix>". Consumers
  // (e.g. TableField empty-state) match the prefix via startsWith so
  // localised strings like "Scanning…" match both "Scanning" and
  // "Scanning…".
  loadingIf: (opt) => {
    const raw = getAfter(opt, 'loadingIf');
    const colon = raw.indexOf(':');
    if (colon < 0) return { key: 'loadingIf', value: undefined };
    return {
      key: 'loadingIf',
      value: { field: raw.slice(0, colon).trim(), val: raw.slice(colon + 1).trim() }
    };
  }
};

// Всі відомі ключі
const allKnownKeys = Array.from(
  new Set(
    Object.values(fieldConfigurations)
      .flatMap((c) => c.options)
      .concat(Object.keys(optionParsers) as FieldOptionKey[])
  )
).sort((a, b) => b.length - a.length);

/**
 * parseFieldOptions(label, optionsString):
 * - Розбиває optionsString по ';'
 * - Зшиває segments для "lines=" (особливо, якщо line-конфіги розкидані через ";")
 * - Викликає optionParsers[..] для кожного сегмента
 * - finalType => 'trend','radio','dropdown','text','slider',...
 */
export function parseFieldOptions(label: string, optionsString: string): ParsedFieldOptions {
  // Universal label override. Backend can ship `l=<custom>` in the
  // option string; we use that verbatim instead of auto-titlecasing
  // the snake_case JSON key. Lets services say `label("CPU Frequency")`
  // to fix acronym/composite-label readability without renaming the
  // JSON key on the wire.
  const explicitLabel = (() => {
    if (!optionsString) return undefined;
    for (const seg of optionsString.split(';')) {
      const s = seg.trim();
      if (s.startsWith('l=')) return s.slice(2);
    }
    return undefined;
  })();
  const autoLabel = label
    .replace(/_/g, ' ')
    .replace(/(?:^|\s)\S/g, (m) => m.toUpperCase());
  const readableLabel = explicitLabel ?? autoLabel;
  const labelOverridden = explicitLabel !== undefined;

  if (!optionsString) {
    return {
      readableLabel,
      labelOverridden,
      optionMap: {},
      finalType: 'text'
    };
  }

  let splitted = optionsString.split(';').map((s) => s.trim()).filter(Boolean);

  // Визначення типу (враховуючи доданий 'slider')
  const supportedTypes = Object.keys(fieldConfigurations) as FieldType[];
  const foundType = splitted.find((opt) => supportedTypes.includes(opt as FieldType)) as FieldType | undefined;
  const finalType = foundType || 'text';

  const config = fieldConfigurations[finalType];

  // Злипаємо "lines="
  const merged: string[] = [];
  for (let i = 0; i < splitted.length; i++) {
    const seg = splitted[i];
    if (seg.startsWith('lines=')) {
      let linesFull = seg;
      while (i + 1 < splitted.length) {
        const nextSeg = splitted[i + 1];
        const knownPrefix = allKnownKeys.some((k) => nextSeg.startsWith(k + '=') || nextSeg === k);
        if (!knownPrefix && nextSeg.includes(':')) {
          linesFull += ';' + nextSeg;
          i++;
        } else {
          break;
        }
      }
      merged.push(linesFull);
    } else {
      merged.push(seg);
    }
  }

  const optionMap: Partial<Record<FieldOptionKey, ParsedFieldOption>> = {};

  merged.forEach((option) => {
    const prefix = allKnownKeys.find((k) => option === k || option.startsWith(`${k}=`));
    if (!prefix) return;

    if (optionParsers[prefix]) {
      const parsed = optionParsers[prefix](option);
      // Перевіряємо відповідність дозволеним опціям для даного типу
      if ((config.options as readonly string[]).includes(parsed.key)) {
        optionMap[parsed.key as FieldOptionKey] = parsed;
      }
    } else if ((config.options as readonly string[]).includes(prefix as FieldOptionKey)) {
      // Ключ без '=true', наприклад "legend"
      optionMap[prefix as FieldOptionKey] = { key: prefix as FieldOptionKey, value: true };
    }
  });

  return {
    readableLabel,
    labelOverridden,
    optionMap,
    finalType
  };
}
