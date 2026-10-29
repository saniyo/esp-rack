import React, { FC, useEffect, useRef, useState } from 'react';
import { Box, Paper, Typography, keyframes, useTheme } from '@mui/material';

import { Field } from '../types';
import { useFieldParser } from '../utils/useFieldParser';
import { FieldAvatar } from '../utils/iconRegistry';
import { FlowEdgeSpec, FlowNodeSpec } from '../utils/fieldParser';
import { tileText } from './TileField';

// Power flow (web::Flow): nodes in a row (a column under NARROW px), an
// edge between neighbours. Every value comes from a sibling key through
// getSibling (live state first, then the form), as refLine does; the
// manifest only names the keys. Edge direction from its dir key: +1 = to ->
// from (blue, e.g. charge), -1 = from -> to (orange, discharge), 0 = idle
// (grey, no arrow). Dashes move with the flow (CSS).
interface FlowFieldProps {
  field: Field;
  getSibling?: (key: string) => any;
}

const NARROW = 420;
const DASH = 14;  // px: one dash + gap of the moving line

export type FlowDir = 1 | -1 | 0;

// A dir key's value as a direction: its sign, 0 for anything else.
export const flowDir = (raw: any): FlowDir => {
  const n = typeof raw === 'number' ? raw : typeof raw === 'string' ? parseFloat(raw) : NaN;
  if (!Number.isFinite(n) || n === 0) return 0;
  return n > 0 ? 1 : -1;
};

export interface FlowLink {
  edge: FlowEdgeSpec;
  fromFirst: boolean;  // the edge's `from` is the first of the two neighbours
}

// The edge between each pair of neighbours (either way round), or null.
export const flowLinks = (nodes: FlowNodeSpec[], edges: FlowEdgeSpec[]): (FlowLink | null)[] =>
  nodes.slice(1).map((n, i) => {
    const a = nodes[i].id;
    const b = n.id;
    const edge = edges.find((e) => (e.from === a && e.to === b) || (e.from === b && e.to === a));
    return edge ? { edge, fromFirst: edge.from === a } : null;
  });

// Which way the arrow points along the row / column: towards the later
// node (+1), the earlier one (-1), or none (idle).
export const flowHeading = (dir: FlowDir, fromFirst: boolean): FlowDir => {
  if (dir === 0) return 0;
  const toLater = dir === -1 ? fromFirst : !fromFirst;  // -1: from -> to
  return toLater ? 1 : -1;
};

const moveX = keyframes`from { background-position: 0 0; } to { background-position: ${DASH}px 0; }`;
const moveY = keyframes`from { background-position: 0 0; } to { background-position: 0 ${DASH}px; }`;

const text = (v: any): string => (v === undefined || v === null ? '' : String(v));

interface EdgeProps {
  link: FlowLink | null;
  vertical: boolean;
  getSibling?: (key: string) => any;
}

const FlowEdge: FC<EdgeProps> = ({ link, vertical, getSibling }) => {
  const theme = useTheme();
  if (!link) return <Box sx={{ flex: '0 0 16px', minHeight: 16 }} />;
  const { edge, fromFirst } = link;
  const dir = flowDir(edge.dirKey ? getSibling?.(edge.dirKey) : 0);
  const heading = flowHeading(dir, fromFirst);
  const color =
    dir > 0 ? theme.palette.info.main : dir < 0 ? theme.palette.warning.main : theme.palette.text.disabled;
  const value = edge.valueKey ? text(getSibling?.(edge.valueKey)) : '';
  const sub = edge.subKey ? text(getSibling?.(edge.subKey)) : '';

  const dashes = `repeating-linear-gradient(${vertical ? '180deg' : '90deg'}, ${color} 0 ${DASH / 2}px, transparent ${DASH / 2}px ${DASH}px)`;
  const line = (
    <Box
      sx={{
        flex: 1,
        ...(vertical ? { width: 3, minHeight: 28 } : { height: 3, minWidth: 24 }),
        borderRadius: 2,
        backgroundImage: dashes,
        animation: heading ? `${vertical ? moveY : moveX} 0.8s linear infinite` : 'none',
        animationDirection: heading < 0 ? 'reverse' : 'normal',
        '@media (prefers-reduced-motion: reduce)': { animation: 'none' },
      }}
    />
  );
  // A CSS triangle at the end the flow goes to.
  const s = 7;
  const tip = (
    <Box
      sx={{
        width: 0,
        height: 0,
        flex: '0 0 auto',
        ...(vertical
          ? { borderLeft: `${s}px solid transparent`, borderRight: `${s}px solid transparent`, [heading > 0 ? 'borderTop' : 'borderBottom']: `${s + 3}px solid ${color}` }
          : { borderTop: `${s}px solid transparent`, borderBottom: `${s}px solid transparent`, [heading > 0 ? 'borderLeft' : 'borderRight']: `${s + 3}px solid ${color}` }),
      }}
    />
  );
  const track = (
    <Box
      sx={{
        display: 'flex',
        alignItems: 'center',
        ...(vertical ? { flexDirection: 'column', flex: '0 0 auto' } : { flexDirection: 'row', flex: 1 }),
      }}
    >
      {heading < 0 && tip}
      {line}
      {heading > 0 && tip}
    </Box>
  );
  const valueEl = value && (
    <Typography variant="body2" sx={{ fontWeight: 600, color, overflowWrap: 'anywhere' }}>
      {value}
      {edge.unit ? ` ${edge.unit}` : ''}
    </Typography>
  );
  const subEl = sub && (
    <Typography variant="caption" color="text.secondary" sx={{ display: 'block', whiteSpace: 'pre-line', overflowWrap: 'anywhere' }}>
      {sub}
    </Typography>
  );

  // Column: the line on the left, the texts next to it. Row: the value
  // over the line, the sub text under it.
  return vertical ? (
    <Box sx={{ display: 'flex', alignItems: 'stretch', justifyContent: 'center', gap: 1.5, minHeight: 56, py: 0.5 }}>
      {track}
      <Box sx={{ display: 'flex', flexDirection: 'column', justifyContent: 'center', minWidth: 0 }}>
        {valueEl}
        {subEl}
      </Box>
    </Box>
  ) : (
    <Box sx={{ flex: '1 1 0', minWidth: 72, maxWidth: 200, display: 'flex', flexDirection: 'column', justifyContent: 'center', px: 0.5, textAlign: 'center' }}>
      <Box sx={{ minHeight: 20 }}>{valueEl}</Box>
      <Box sx={{ display: 'flex', alignItems: 'center', height: 14, my: 0.5 }}>{track}</Box>
      <Box sx={{ minHeight: 20 }}>{subEl}</Box>
    </Box>
  );
};

const FlowNode: FC<{ node: FlowNodeSpec; getSibling?: (key: string) => any }> = ({ node, getSibling }) => {
  const value = node.valueKey ? tileText(getSibling?.(node.valueKey), undefined, undefined) : '';
  const sub = node.subKey ? text(getSibling?.(node.subKey)) : '';
  return (
    <Paper
      variant="outlined"
      sx={{ flex: '1 1 0', minWidth: 0, p: 1.5, display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 0.5, textAlign: 'center' }}
    >
      <FieldAvatar iconName={node.icon} />
      <Typography variant="body2" color="text.secondary" sx={{ overflowWrap: 'anywhere' }}>
        {node.title}
      </Typography>
      {value && (
        <Box sx={{ display: 'flex', alignItems: 'baseline', justifyContent: 'center', flexWrap: 'wrap', columnGap: 0.5 }}>
          <Typography component="span" sx={{ fontSize: '1.5rem', fontWeight: 600, lineHeight: 1.2, overflowWrap: 'anywhere' }}>
            {value}
          </Typography>
          {node.unit && value !== '—' && (
            <Typography component="span" variant="body2" color="text.secondary">
              {node.unit}
            </Typography>
          )}
        </Box>
      )}
      {sub && (
        <Typography variant="body2" color="text.secondary" sx={{ whiteSpace: 'pre-line', overflowWrap: 'anywhere' }}>
          {sub}
        </Typography>
      )}
    </Paper>
  );
};

const FlowField: FC<FlowFieldProps> = ({ field, getSibling }) => {
  const { readableLabel, labelOverridden, optionMap } = useFieldParser(field.label, field.o || '');
  const om = optionMap as any;
  const nodes = (om.nodes?.value as FlowNodeSpec[] | undefined) || [];
  const edges = (om.edges?.value as FlowEdgeSpec[] | undefined) || [];
  const links = flowLinks(nodes, edges);

  const boxRef = useRef<HTMLDivElement>(null);
  const [width, setWidth] = useState(NARROW);
  useEffect(() => {
    const el = boxRef.current;
    if (!el || typeof ResizeObserver === 'undefined') return;
    const ro = new ResizeObserver((e) => setWidth(e[0].contentRect.width));
    ro.observe(el);
    return () => ro.disconnect();
  }, []);
  const vertical = width < NARROW;

  return (
    <Box component="li" sx={{ py: 1 }}>
      {labelOverridden && (
        <Typography variant="subtitle1" sx={{ mb: 1 }}>
          {readableLabel}
        </Typography>
      )}
      <Box ref={boxRef} sx={{ display: 'flex', flexDirection: vertical ? 'column' : 'row', alignItems: 'stretch' }}>
        {nodes.map((n, i) => (
          <React.Fragment key={n.id}>
            {i > 0 && <FlowEdge link={links[i - 1]} vertical={vertical} getSibling={getSibling} />}
            <FlowNode node={n} getSibling={getSibling} />
          </React.Fragment>
        ))}
      </Box>
    </Box>
  );
};

export default FlowField;
