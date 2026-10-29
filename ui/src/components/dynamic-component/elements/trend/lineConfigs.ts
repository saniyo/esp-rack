// The series of a trend (lines=key:color=…,type=…;…) and the signed area
// under a line (ap=/an=, .signedArea(pos, neg) in Form.h).

export type CustomCurveType =
  | 'linear'
  | 'monotone'
  | 'basis'
  | 'basisClosed'
  | 'basisOpen'
  | 'natural'
  | 'step'
  | 'stepBefore'
  | 'stepAfter';

export interface LineConfig {
  dataKey: string;
  readableLabel: string;
  color: string;
  type: CustomCurveType;
  hidden: boolean;
  histSpec?: string;  // h=: the history column (default: dataKey)
  histSign: number;   // hk=-1: drawn below 0, like the live push
  // ap=/an=: the area under the line, one colour above 0 and another below
  // (.signedArea(pos, neg)); a lineChart with one renders as a ComposedChart.
  areaPos?: string;
  areaNeg?: string;
}

/** ************************************************** **
 *        ФУНКЦІЯ ПАРСИНГУ lines=... -> LineConfig[]
 ** ************************************************** **/
export function parseLineConfigs(
  linesString: string,
  labelMap: Record<string, string>
): LineConfig[] {
  if (!linesString) return [];

  const predefinedColors = [
    '#8884d8',
    '#FF0000',
    '#FF00FF',
    '#00C49F',
    '#FFBB28',
    '#FF8042',
    '#8A2BE2',
    '#5F9EA0',
    '#D2691E',
    '#FF7F50',
    '#6495ED',
    '#DC143C',
    '#00FFFF',
    '#00008B',
    '#008B8B',
    '#B8860B',
    '#A9A9A9',
    '#006400',
    '#BDB76B',
    '#8B008B',
    '#556B2F',
  ];
  const getColor = (idx: number) => predefinedColors[idx % predefinedColors.length];

  // lines=key19:color=#123456,type=step;key20:color=#abc123,hidden=true ...
  return linesString.split(';').map((part, index) => {
    const [dataKeyRaw, rest] = part.split(':');
    const dataKey = (dataKeyRaw || '').trim();

    let hidden = false;
    let color = getColor(index);
    let type: CustomCurveType = 'monotone';
    let name = '';
    let histSpec: string | undefined;
    let histSign = 1;
    let areaPos: string | undefined;
    let areaNeg: string | undefined;

    if (rest) {
      // наприклад "color=#ff0000,type=step"
      const params = rest.split(',');
      params.forEach((param) => {
        const [k, v] = param.split('=');
        if (!k || !v) return;
        const key = k.trim();
        const value = v.trim();
        if (key === 'hidden' && value === 'true') {
          hidden = true;
        } else if (key === 'color') {
          color = value;
        } else if (key === 'type') {
          type = value as CustomCurveType;
        } else if (key === 'name') {
          name = value;
        } else if (key === 'h') {
          histSpec = value;
        } else if (key === 'hk') {
          histSign = Number(value) < 0 ? -1 : 1;
        } else if (key === 'ap') {
          areaPos = value;
        } else if (key === 'an') {
          areaNeg = value;
        }
      });
    }

    return {
      dataKey,
      readableLabel: name || labelMap[dataKey] || dataKey,
      hidden,
      color,
      type,
      histSpec,
      histSign,
      // one colour given: both sides take it
      ...(areaPos || areaNeg ? { areaPos: areaPos || areaNeg, areaNeg: areaNeg || areaPos } : {}),
    };
  });
}

// The gradient stop of a signed area: where 0 sits in the area's box (the
// span between 0 and the data's extremes), 0 = top, 1 = bottom. All above
// 0 gives 1 (one colour), all below 0 gives 0.
export function signedAreaOffset(rows: Record<string, any>[], key: string): number {
  let hi = 0;
  let lo = 0;
  rows.forEach((r) => {
    const v = r[key];
    if (typeof v === 'number' && Number.isFinite(v)) {
      if (v > hi) hi = v;
      if (v < lo) lo = v;
    }
  });
  return hi - lo > 0 ? hi / (hi - lo) : 1;
}

// The curve of a signed area. The gradient stop above assumes the area's box
// spans exactly 0..the data's extremes; linear, monotone and the steps pass
// through every point and stay within them, but basis* never reaches the
// extremes and natural overshoots them, which would move the colour split
// off 0. Those take monotone for the area (the line keeps its own curve).
export function signedAreaCurve(type: CustomCurveType): CustomCurveType {
  return type.startsWith('basis') || type === 'natural' ? 'monotone' : type;
}

// The rows a trend tooltip lists: the series a chart marked tooltipType="none"
// (the signed area) are left out — our own tooltip content does not get
// recharts' default filter — then, on a hovered legend entry, only that series.
export function tooltipEntries<T extends { type?: string; dataKey?: any }>(
  payload: T[],
  hoveredDataKey: string | null
): T[] {
  const visible = payload.filter((e) => e.type !== 'none');
  return hoveredDataKey ? visible.filter((e) => e.dataKey === hoveredDataKey) : visible;
}

// An area colour: a theme palette key (primary, info, warning, …) takes the
// palette's main colour (light and dark themes), anything else is a CSS colour.
export function areaColor(c: string | undefined, palette: Record<string, any>): string | undefined {
  if (!c) return undefined;
  const p = palette[c];
  return p && typeof p === 'object' && typeof p.main === 'string' ? p.main : c;
}
