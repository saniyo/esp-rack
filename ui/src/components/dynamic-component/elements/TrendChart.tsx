import React, { useEffect, useRef, useState, useMemo, useCallback } from 'react';
import {
  LineChart,
  Line,
  BarChart,
  Bar,
  PieChart,
  Pie,
  Cell,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  Legend,
  ReferenceLine,
  ResponsiveContainer,
  TooltipProps as RechartsTooltipProps,
} from 'recharts';
import {
  ListItem,
  ListItemAvatar,
  ListItemText,
  Divider,
  Box,
} from '@mui/material';
import { FieldAvatar } from '../utils/iconRegistry';

import { parseFieldOptions, ParsedFieldOptions, RefLineSpec } from '../utils/fieldParser';
import { formatFull, formatX, isXFormat, toEpochMs, XFormat } from '../../../utils/deviceTime';
import { useDeviceZone } from '../../../utils/useDeviceZone';

/** ****************************
 *  КОНСТАНТИ ТА ТИПИ
 ***************************** */
type CustomCurveType =
  | 'linear'
  | 'monotone'
  | 'basis'
  | 'basisClosed'
  | 'basisOpen'
  | 'natural'
  | 'step'
  | 'stepBefore'
  | 'stepAfter';

interface LineConfig {
  dataKey: string;
  readableLabel: string;
  color: string;
  type: CustomCurveType;
  hidden: boolean;
}

interface Field {
  label: string;
  o: string; // містить усі опції: mode=barChart; lines=key1:color=...; ...
}

interface DataPoint {
  timestamp: number; // але може бути будь-яке число
  [key: string]: number | string;
}

interface PieChartData {
  dataKey: string;
  name: string;
  value: number;
}

interface TrendChartProps {
  field: Field;
  value: DataPoint[];
  // Sibling state values (refl=key:…): live readState keys or form fields.
  getSibling?: (key: string) => any;
}

/** ****************************
 *  ХЕДЕР ГРАФІКА
 ***************************** */
interface ChartHeaderProps {
  readableLabel: string;
  secondaryText?: string;
  iconName?: string;
}

const ChartHeader = React.memo(({ readableLabel, secondaryText, iconName }: ChartHeaderProps) => (
  <ListItem>
    <ListItemAvatar>
      <FieldAvatar iconName={iconName} />
    </ListItemAvatar>
    <ListItemText primary={readableLabel} secondary={secondaryText} />
  </ListItem>
));

/** ************************************************** **
 *        ФУНКЦІЯ ПАРСИНГУ lines=... -> LineConfig[]
 ** ************************************************** **/
function parseLineConfigs(
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
        }
      });
    }

    return {
      dataKey,
      readableLabel: name || labelMap[dataKey] || dataKey,
      hidden,
      color,
      type,
    };
  });
}

/** ****************************
 *  КАСТОМНИЙ КОМПОНЕНТ ЛЕГЕНДИ
 ***************************** */
interface CustomLegendProps {
  payload: {
    dataKey: string;
    color: string;
    value: string;
    isNegative: boolean;
  }[];
  hiddenLines: Record<string, boolean>;
  onClick?: (dataKey: string) => void;
  interactive: boolean;
}

const CustomLegend: React.FC<CustomLegendProps> = ({
  payload,
  hiddenLines,
  onClick,
  interactive,
}) => {
  return (
    <ul
      style={{
        listStyle: 'none',
        display: 'flex',
        flexWrap: 'wrap',
        padding: 0,
        margin: 0,
      }}
    >
      {payload.map((entry) => {
        const { dataKey, color, value, isNegative } = entry;
        const isHidden = hiddenLines[dataKey];
        return (
          <li
            key={`legend-item-${dataKey}-${value}`}
            onClick={
              interactive && !isNegative && onClick
                ? () => onClick(dataKey)
                : undefined
            }
            style={{
              marginRight: 10,
              cursor: interactive && !isNegative ? 'pointer' : 'default',
              display: 'flex',
              alignItems: 'center',
              color: isHidden ? '#999' : '#000',
              textDecoration: isHidden ? 'line-through' : 'none',
              transition: 'color 0.3s, text-decoration 0.3s',
              opacity: isNegative ? 0.6 : 1,
            }}
          >
            <span
              style={{
                display: 'inline-block',
                backgroundColor: color || '#8884d8',
                width: 10,
                height: 10,
                marginRight: 5,
              }}
            />
            {value}
            {isNegative && (
              <span style={{ marginLeft: 3, fontSize: '10px', color: '#555' }}>
                (Negative)
              </span>
            )}
          </li>
        );
      })}
    </ul>
  );
};

/** ****************************
 *  КАСТОМНИЙ TOOLTIP
 ***************************** */
// Recharts v3 tightened TooltipProps generics so active/payload/label are no
// longer surfaced on the shared type. The content prop in v3 still receives
// those fields at runtime though, so we widen locally with `any` rather
// than trying to chase the upstream type.
interface CustomTooltipProps {
  active?: boolean;
  payload?: any[];
  label?: any;
  hoveredDataKey: string | null;
  formatLabel?: (label: any) => string;
}

// Recharts v3 removed the `coordinate` prop from the tooltip content payload
// and handles positioning via its own wrapper. We now just emit styled
// content; the outer wrapper provided by recharts already takes care of
// location / offset relative to the pointer.
const CustomTooltipComponent: React.FC<CustomTooltipProps> = ({
  active,
  payload,
  label,
  hoveredDataKey,
  formatLabel,
}) => {
  if (active && payload && payload.length) {
    // Якщо користувач "наводить" на конкретну серію (Bar), можемо відфільтрувати
    const filteredPayload = hoveredDataKey
      ? payload.filter((entry: any) => entry.dataKey === hoveredDataKey)
      : payload;

    const tooltipStyle: React.CSSProperties = {
      backgroundColor: '#fff',
      padding: '8px',
      border: '1px solid #ccc',
      borderRadius: '4px',
      boxShadow: '0 2px 8px rgba(0,0,0,0.15)',
      pointerEvents: 'none',
      whiteSpace: 'nowrap',
    };

    return (
      <div style={tooltipStyle}>
        <span
          style={{
            fontWeight: 'bold',
            marginBottom: '4px',
            display: 'block',
          }}
        >
          {formatLabel ? formatLabel(label) : label}
        </span>
        {filteredPayload.map((entry: any, index: number) => (
          <span
            key={`item-${index}`}
            style={{
              color: entry.color || '#8884d8',
              display: 'flex',
              alignItems: 'center',
              fontSize: '12px',
            }}
          >
            <span
              style={{
                display: 'inline-block',
                backgroundColor: entry.color || '#8884d8',
                width: 10,
                height: 10,
                marginRight: 5,
              }}
            />
            {entry.name}: {entry.value}
          </span>
        ))}
      </div>
    );
  }
  return null;
};

/** ********************************************************************
 *   ГОЛОВНИЙ КОМПОНЕНТ TrendChart: підключаємо логіку "MAX_POINTS"
 ********************************************************************* */
const TrendChart: React.FC<TrendChartProps> = ({ field, value = [], getSibling }) => {
  // const theme = useTheme();

  // 1) Розбір опцій
  //    Наприклад: "trend;mode=lineChart;lines=key1:color=red;..."
  const parsedOptions: ParsedFieldOptions = useMemo(
    () => parseFieldOptions(field.label, field.o),
    [field.label, field.o]
  );
  const readableLabel = parsedOptions.readableLabel;

  // SES-LIB-015 options. Without xFmt the x axis shows the raw numbers, as
  // before (millis() stamps must not turn into 1970 dates).
  const om = parsedOptions.optionMap as any;
  const xFmt: XFormat | undefined = isXFormat(om.xFmt?.value) ? om.xFmt.value : undefined;
  const isCategory = !!om.cat;
  const stacks: string[][] = useMemo(() => (Array.isArray(om.stack?.value) ? om.stack.value : []), [om.stack]);
  const refLines: RefLineSpec[] = useMemo(() => (Array.isArray(om.refl?.value) ? om.refl.value : []), [om.refl]);
  const vrefLines: RefLineSpec[] = useMemo(() => (Array.isArray(om.vrefl?.value) ? om.vrefl.value : []), [om.vrefl]);
  const finite = (o: any): number | undefined =>
    typeof o?.value === 'number' && Number.isFinite(o.value) ? o.value : undefined;
  const yMin = finite(om.ymin);
  const yMax = finite(om.ymax);
  const yPad = finite(om.ypad);
  const zone = useDeviceZone();

  // Шукаємо "mode=..." (lineChart / barChart / pieChart)
  const modeMatch = field.o.match(/mode=([^;]+)/);
  const rawMode: string = modeMatch ? modeMatch[1] : 'lineChart';

  // Якщо в opціях "xAxis=..." — беремо, інакше "timestamp"
  const xAxisMatch = field.o.match(/xAxis=([^;]+)/);
  const xAxisKey: string = xAxisMatch ? xAxisMatch[1] : 'timestamp';

  // lines=... — the field parser has already joined the series, which are
  // separated by the same ';' as the options.
  const linesStr: string = (om.lines?.value as string) || '';

  // maxPoints from the field's options string (C++ side passes it via
  // trendMaxPoints(N)). Caps both the input window AND the accumulated map,
  // so LineChart gets the "oscilloscope" sliding window — new point on the
  // right, oldest falls off the left, no bulk redraw.
  const MAX_POINTS = useMemo(() => {
    const m = field.o.match(/maxPoints=(\d+)/);
    const n = m ? Number(m[1]) : 0;
    return n > 0 ? n : 3000;
  }, [field.o]);

  // ***************************************************
  // (A) Робимо стейт mergedDataMap[timestamp] = {...}
  // ***************************************************
  const [mergedDataMap, setMergedDataMap] = useState<
    Record<number, Record<string, number>>
  >({});

  useEffect(() => {
    if (isCategory) return;  // category data is replaced, never merged by timestamp
    if (!Array.isArray(value) || value.length === 0) return;

    setMergedDataMap((prev) => {
      const newMap = { ...prev };
      let changed = false;

      value.forEach((pt) => {
        const t = pt.timestamp;
        if (typeof t !== 'number') return;

        if (!newMap[t]) {
          newMap[t] = {};
          changed = true;
        }
        Object.keys(pt).forEach((k) => {
          if (k === 'timestamp') return;
          const val = pt[k];
          if (typeof val === 'number') {
            if (newMap[t][k] !== val) {
              newMap[t][k] = val;
              changed = true;
            }
          }
        });
      });

      if (changed) {
        // Обрізаємо, щоб не перевищити MAX_POINTS
        const sortedTimestamps = Object.keys(newMap)
          .map(Number)
          .sort((a, b) => a - b);

        if (sortedTimestamps.length > MAX_POINTS) {
          const extra = sortedTimestamps.length - MAX_POINTS;
          const toRemove = sortedTimestamps.slice(0, extra); // найстаріші
          toRemove.forEach((ts) => {
            delete newMap[ts];
          });
        }
        return newMap;
      }

      return prev;
    });
  }, [value, MAX_POINTS, isCategory]);

  // Формуємо масив для лінійного графіка: [{timestamp,...},...]
  const chartDataForLine = useMemo<Record<string, any>[]>(() => {
    // Category mode: the pushed rows ARE the data (x = the xAxis key's value).
    if (isCategory) return Array.isArray(value) ? (value as Record<string, any>[]) : [];
    const timestamps = Object.keys(mergedDataMap)
      .map(Number)
      .sort((a, b) => a - b);
    return timestamps.map((ts) => ({
      [xAxisKey]: ts,
      ...mergedDataMap[ts],
    }));
  }, [mergedDataMap, xAxisKey, isCategory, value]);

  // Visible x span in seconds — xFmt "epoch" picks time vs date by it.
  const spanSec = useMemo<number | undefined>(() => {
    const xs = chartDataForLine.map((r) => r[xAxisKey]).filter((x) => typeof x === 'number') as number[];
    if (xs.length < 2) return undefined;
    return (toEpochMs(Math.max(...xs)) - toEpochMs(Math.min(...xs))) / 1000;
  }, [chartDataForLine, xAxisKey]);

  const tickFormatter = useMemo(
    () => (xFmt ? (x: any) => formatX(x, xFmt, zone, spanSec) : undefined),
    [xFmt, zone, spanSec]
  );
  const tooltipLabel = useMemo(
    () => (xFmt ? (x: any) => formatFull(x, xFmt, zone) : undefined),
    [xFmt, zone]
  );

  // ***************************************************
  // (B) Snapshot-of-latest for pieChart only.
  // BarChart now consumes chartDataForLine directly (time-series mode),
  // so no Latest aggregation is needed for bars.
  // ***************************************************
  const latestValuesRef = useRef<Record<string, number>>({});
  const [pieChartData, setPieChartData] = useState<PieChartData[]>([]);

  useEffect(() => {
    if (!Array.isArray(value) || value.length === 0) return;

    let changed = false;
    const copyLatest = { ...latestValuesRef.current };

    value.forEach((pt) => {
      Object.keys(pt).forEach((k) => {
        if (k === 'timestamp') return;
        const val = pt[k];
        if (typeof val === 'number') {
          if (copyLatest[k] !== val) {
            copyLatest[k] = val;
            changed = true;
          }
        }
      });
    });

    if (changed) {
      latestValuesRef.current = copyLatest;
      const allKeys = Object.keys(copyLatest).sort();

      const newPieData: PieChartData[] = allKeys.map((key) => ({
        dataKey: key,
        name: key,
        value: copyLatest[key],
      }));
      setPieChartData(newPieData);
    }
  }, [value]);

  // ***************************************************
  // (C) Перевірка, чи є ключі з негативними значеннями (для легенди)
  // ***************************************************
  const isKeyNegative = useMemo<Record<string, boolean>>(() => {
    const map: Record<string, boolean> = {};
    pieChartData.forEach((data) => {
      map[data.dataKey] = data.value < 0;
    });
    return map;
  }, [pieChartData]);

  // ***************************************************
  // (D) Формуємо lineConfigs (з lines=...) + все, що є у mergedDataMap
  // ***************************************************
  // labelMap для parseLineConfigs
  const labelMap: Record<string, string> = useMemo(() => {
    const map: Record<string, string> = {};
    Object.entries(parsedOptions.optionMap).forEach(([key, option]) => {
      if (typeof option.value === 'string') {
        map[key] = option.value;
      } else if (Array.isArray(option.value)) {
        map[key] = option.value.join(', ');
      } else {
        map[key] = key;
      }
    });
    return map;
  }, [parsedOptions.optionMap]);

  // парсимо lines=key19:color=...,type=...,hidden=true;key20:color=..., ...
  const baseLineConfigs = useMemo(
    () => parseLineConfigs(linesStr, labelMap),
    [linesStr, labelMap]
  );

  // Збираємо усі ключі фактично з mergedDataMap + latestValuesRef
  const mergedLineConfigs = useMemo<LineConfig[]>(() => {
    const allKeysSet = new Set<string>();
    if (isCategory) {
      // series = every numeric key of the rows except the category itself
      chartDataForLine.forEach((row) => {
        Object.keys(row).forEach((k) => {
          if (k !== xAxisKey && k !== 'timestamp' && typeof row[k] === 'number') allKeysSet.add(k);
        });
      });
    } else {
      // (1) додамо ключі з mergedDataMap
      Object.values(mergedDataMap).forEach((row) => {
        Object.keys(row).forEach((k) => allKeysSet.add(k));
      });
      // (2) додамо ключі з latestValuesRef
      Object.keys(latestValuesRef.current).forEach((k) => allKeysSet.add(k));
    }

    const allKeys = Array.from(allKeysSet).sort();

    // створимо map для швидкого пошуку
    const configMap: Record<string, LineConfig> = {};
    baseLineConfigs.forEach((cfg) => {
      configMap[cfg.dataKey] = cfg;
    });

    // Призначимо колір, type тощо тим ключам, яких немає в lines=...
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
    const getColor = (i: number) => predefinedColors[i % predefinedColors.length];

    return allKeys.map((key, idx) => {
      const existingCfg = configMap[key];
      if (existingCfg) {
        return existingCfg;
      }
      return {
        dataKey: key,
        readableLabel: key,
        hidden: false,
        color: getColor(idx),
        type: 'monotone' as CustomCurveType,
      };
    });
  }, [baseLineConfigs, mergedDataMap, isCategory, chartDataForLine, xAxisKey]);

  // ***************************************************
  // (E) Стан прихованих ліній (show/hide), керуємо через легенду
  // ***************************************************
  const [hiddenLines, setHiddenLines] = useState<Record<string, boolean>>({});

  useEffect(() => {
    setHiddenLines((prev) => {
      const newHidden = { ...prev };
      mergedLineConfigs.forEach((cfg) => {
        if (!(cfg.dataKey in newHidden)) {
          newHidden[cfg.dataKey] = cfg.hidden;
        }
      });
      return newHidden;
    });
  }, [mergedLineConfigs]);

  const handleLegendClick = useCallback((dataKey: string) => {
    setHiddenLines((prev) => ({
      ...prev,
      [dataKey]: !prev[dataKey],
    }));
  }, []);

  // ***************************************************
  // (F) Управління tooltip при hover на Bar
  // ***************************************************
  const [hoveredDataKey, setHoveredDataKey] = useState<string | null>(null);
  const handleBarMouseEnter = useCallback((dataKey: string) => {
    setHoveredDataKey(dataKey);
  }, []);
  const handleBarMouseLeave = useCallback(() => {
    setHoveredDataKey(null);
  }, []);

  // ***************************************************
  // (G) Рендер ліній / барів / pie
  // ***************************************************
  const renderLines = useMemo(() => {
    return mergedLineConfigs.map((cfg) => (
      <Line
        key={cfg.dataKey}
        type={cfg.type}
        dataKey={cfg.dataKey}
        name={cfg.readableLabel}
        stroke={cfg.color}
        strokeWidth={2}
        dot={false}
        // Animation OFF: at 10 Hz pushes each animation ran while the next
        // point was already in — caused visible flicker/redraw. With it off
        // the chart behaves like an oscilloscope: new point in on the right,
        // oldest falls off the left, no easing in between.
        isAnimationActive={false}
        hide={hiddenLines[cfg.dataKey]}
      />
    ));
  }, [mergedLineConfigs, hiddenLines]);

  const renderBars = useMemo(() => {
    // stack=a+b|c+d: bars of one group share a stackId.
    const stackOf: Record<string, string> = {};
    stacks.forEach((g, i) => g.forEach((k) => { stackOf[k] = `s${i}`; }));
    return mergedLineConfigs.map((cfg) => (
      <Bar
        key={cfg.dataKey}
        dataKey={cfg.dataKey}
        name={cfg.readableLabel}
        fill={cfg.color}
        stackId={stackOf[cfg.dataKey]}
        hide={hiddenLines[cfg.dataKey]}
        isAnimationActive={false}
      />
    ));
  }, [mergedLineConfigs, hiddenLines, stacks]);

  // refl=key:color:label — a horizontal line at the sibling's live value;
  // hidden while that value is missing or not a number.
  const sibVals = refLines.map((r) => getSibling?.(r.key));
  const sibKey = JSON.stringify(sibVals);
  const renderRefLines = useMemo(() => {
    return refLines
      .map((r, i) => {
        const raw = sibVals[i];
        const y = typeof raw === 'number' ? raw : typeof raw === 'string' ? parseFloat(raw) : NaN;
        if (!Number.isFinite(y)) return null;
        return (
          <ReferenceLine
            key={`refl-${r.key}`}
            y={y}
            stroke={r.color || '#888'}
            strokeDasharray="4 4"
            ifOverflow="extendDomain"
            label={r.label ? { value: r.label, position: 'insideTopRight', fontSize: 12 } : undefined}
          />
        );
      })
      .filter(Boolean);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [refLines, sibKey]);

  // vrefl=key:color:label — a vertical line at the x the sibling's live value
  // names: on a category axis the category equal to it, else the nearest x
  // in the data. Hidden while the value is missing or outside the data. The
  // label on top reads "label value".
  const vsibVals = vrefLines.map((r) => getSibling?.(r.key));
  const vsibKey = JSON.stringify(vsibVals);
  const renderVRefLines = useMemo(() => {
    const xs = chartDataForLine.map((r) => r[xAxisKey]);
    const numeric = !isCategory && xs.length > 0 && xs.every((v) => typeof v === 'number');
    return vrefLines
      .map((r, i) => {
        const raw = vsibVals[i];
        if (raw === undefined || raw === null || raw === '') return null;
        let x: any;
        if (numeric) {
          const n = typeof raw === 'number' ? raw : parseFloat(raw);
          if (!Number.isFinite(n) || n < xs[0] || n > xs[xs.length - 1]) return null;
          x = xs.reduce((best, v) => (Math.abs(v - n) < Math.abs(best - n) ? v : best), xs[0]);
        } else {
          x = xs.find((v) => String(v) === String(raw));
          if (x === undefined) return null;
        }
        const shown = numeric && tickFormatter ? tickFormatter(x) : String(raw);
        const color = r.color || '#888';
        return (
          <ReferenceLine
            key={`vrefl-${r.key}`}
            x={x}
            stroke={color}
            strokeDasharray="4 4"
            label={{ value: r.label ? `${r.label} ${shown}` : shown, position: 'top', fontSize: 11, fill: color }}
          />
        );
      })
      .filter(Boolean);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [vrefLines, vsibKey, chartDataForLine, xAxisKey, isCategory, tickFormatter]);

  // ymin/ymax fix an end of the y axis. ypad lets the free ends follow the
  // visible data with that margin (a fraction of its span), rounded to a
  // tidy step; without it a free end is recharts' 'auto'. With none of them
  // the axis stays as before (from 0).
  const yDomain = useMemo<[number | string, number | string] | undefined>(() => {
    if (yMin === undefined && yMax === undefined && yPad === undefined) return undefined;
    if (yPad === undefined) return [yMin ?? 'auto', yMax ?? 'auto'];
    let lo = Infinity;
    let hi = -Infinity;
    const keys = mergedLineConfigs.filter((c) => !hiddenLines[c.dataKey]).map((c) => c.dataKey);
    chartDataForLine.forEach((row) =>
      keys.forEach((k) => {
        const v = row[k];
        if (typeof v === 'number' && Number.isFinite(v)) {
          lo = Math.min(lo, v);
          hi = Math.max(hi, v);
        }
      })
    );
    if (lo > hi) return [yMin ?? 'auto', yMax ?? 'auto'];
    const span = hi - lo || Math.abs(hi) || 1;
    const step = Math.pow(10, Math.floor(Math.log10(span)) - 1);
    const tidy = (v: number) => Number(v.toPrecision(12));
    return [
      yMin ?? tidy(Math.floor((lo - yPad * span) / step) * step),
      yMax ?? tidy(Math.ceil((hi + yPad * span) / step) * step),
    ];
  }, [yMin, yMax, yPad, mergedLineConfigs, hiddenLines, chartDataForLine]);
  const chartMargin = useMemo(
    () => ({ top: vrefLines.length ? 20 : 5, right: 30, left: 20, bottom: 5 }),
    [vrefLines.length]
  );

  const renderPie = useMemo(() => {
    // Для прикладу фільтруємо все, що "приховане" або зі значенням < 0 (можна змінити логіку)
    const visiblePieData = pieChartData.filter((data) => {
      return !hiddenLines[data.dataKey] && data.value > 0;
    });

    if (visiblePieData.length === 0) {
      return <div>Немає даних для відображення.</div>;
    }

    return (
      <>
        <Tooltip
          content={(props: RechartsTooltipProps<any, any>) => (
            <CustomTooltipComponent
              {...props}
              hoveredDataKey={null}
            />
          )}
        />
        <Pie
          data={visiblePieData}
          dataKey="value"
          nameKey="name"
          cx="50%"
          cy="50%"
          outerRadius={150}
          label={false}
          stroke="#000000"
          strokeWidth={1}
          // Animation OFF: at 10 Hz pie data changes the tween objects never
          // settle and pile up as allocation churn in recharts. Bar/Line
          // already use the same pattern — keep them consistent.
          isAnimationActive={false}
        >
          {visiblePieData.map((entry, index) => {
            const config = mergedLineConfigs.find(
              (cfg) => cfg.dataKey === entry.dataKey
            );
            const color = config?.color || '#8884d8';
            return <Cell key={`cell-${index}`} fill={color} />;
          })}
        </Pie>
        <Legend
          verticalAlign="bottom"
          height={36}
          content={
            <CustomLegend
              payload={visiblePieData.map((data) => {
                const config = mergedLineConfigs.find(
                  (cfg) => cfg.dataKey === data.dataKey
                );
                return {
                  dataKey: data.dataKey,
                  color: config?.color || '#8884d8',
                  value: config?.readableLabel || data.name,
                  isNegative: isKeyNegative[data.dataKey] || false,
                };
              })}
              hiddenLines={{}} // Не робимо інтерактивним Pie
              interactive={false}
            />
          }
        />
      </>
    );
  }, [pieChartData, mergedLineConfigs, hiddenLines, isKeyNegative]);

  // ***************************************************
  // (H) Обираємо потрібний тип графіка
  // ***************************************************
  const renderedChart = useMemo(() => {
    switch (rawMode) {
      case 'lineChart':
        return (
          <LineChart
            data={chartDataForLine}
            margin={chartMargin}
          >
            <CartesianGrid strokeDasharray="3 3" />
            <XAxis dataKey={xAxisKey} tickFormatter={tickFormatter} />
            <YAxis domain={yDomain} />
            <Tooltip
              content={(props: RechartsTooltipProps<any, any>) => (
                <CustomTooltipComponent
                  {...props}
                  hoveredDataKey={hoveredDataKey}
                  formatLabel={tooltipLabel}
                />
              )}
            />
            <Legend
              content={
                <CustomLegend
                  payload={mergedLineConfigs.map((cfg) => ({
                    dataKey: cfg.dataKey,
                    color: cfg.color,
                    value: cfg.readableLabel,
                    isNegative: isKeyNegative[cfg.dataKey],
                  }))}
                  hiddenLines={hiddenLines}
                  onClick={handleLegendClick}
                  interactive={true}
                />
              }
            />
            {renderLines}
            {renderRefLines}
            {renderVRefLines}
          </LineChart>
        );

      case 'barChart':
        // Time-series bar mode: one bar cluster per timestamp (sharing the
        // same maxPoints window as LineChart). Hovering a bar shows the
        // tooltip for that single point — no more "Latest" carpet where
        // every series piles into one column.
        return (
          <BarChart
            data={chartDataForLine}
            margin={chartMargin}
            barCategoryGap="20%"
          >
            <CartesianGrid strokeDasharray="3 3" />
            <XAxis dataKey={xAxisKey} tickFormatter={tickFormatter} />
            <YAxis domain={yDomain} />
            <Tooltip
              content={(props: RechartsTooltipProps<any, any>) => (
                <CustomTooltipComponent
                  {...props}
                  hoveredDataKey={hoveredDataKey}
                  formatLabel={tooltipLabel}
                />
              )}
            />
            <Legend
              content={
                <CustomLegend
                  payload={mergedLineConfigs.map((cfg) => ({
                    dataKey: cfg.dataKey,
                    color: cfg.color,
                    value: cfg.readableLabel,
                    isNegative: isKeyNegative[cfg.dataKey],
                  }))}
                  hiddenLines={hiddenLines}
                  onClick={handleLegendClick}
                  interactive={true}
                />
              }
            />
            {renderBars.map((bar) =>
              React.cloneElement(bar, {
                onMouseEnter: () => handleBarMouseEnter(bar.props.dataKey),
                onMouseLeave: handleBarMouseLeave,
              })
            )}
            {renderRefLines}
            {renderVRefLines}
          </BarChart>
        );

      case 'pieChart':
        return (
          <PieChart margin={{ top: 5, right: 30, left: 20, bottom: 5 }}>
            {renderPie}
          </PieChart>
        );

      default:
        return <div>Невідомий режим графіка: {rawMode}</div>;
    }
  }, [
    rawMode,
    chartDataForLine,
    mergedLineConfigs,
    renderLines,
    renderBars,
    handleLegendClick,
    hoveredDataKey,
    handleBarMouseEnter,
    handleBarMouseLeave,
    isKeyNegative,
    xAxisKey,
    renderPie,
    hiddenLines,
    tickFormatter,
    tooltipLabel,
    renderRefLines,
    renderVRefLines,
    yDomain,
    chartMargin,
  ]);

  // ***************************************************
  // (I) Остаточний return
  // ***************************************************
  return (
    <>
      <ChartHeader
        readableLabel={readableLabel}
        secondaryText={rawMode}
        iconName={(parsedOptions.optionMap as any).icon?.value as string | undefined}
      />
      <ListItem disableGutters>
        <Box
          sx={{
            width: '100%',
            height: 400,
            border: '1px solid #ccc',
            position: 'relative',
            overflow: 'visible',
            backgroundColor: '#ffffff',
          }}
        >
          <ResponsiveContainer width="100%" height="100%">
            {renderedChart}
          </ResponsiveContainer>
        </Box>
      </ListItem>
      <Divider variant="inset" component="li" />
    </>
  );
};

export default TrendChart;
