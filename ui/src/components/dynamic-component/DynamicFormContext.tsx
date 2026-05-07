import { createContext, useContext } from 'react';

// Context surfaced by each DynamicTabPanel so its descendants (ActionField
// wanting to refetch after an action fires, TableField wanting to show a
// loader during a background scan, etc.) can coordinate without explicit
// prop drilling.
export interface DynamicFormCtx {
  // Triggers a fresh GET of the tab's restPath. With silent=true the
  // underlying useRest keeps previously rendered data in place during
  // the fetch (no placeholder flash) and swallows errors (used by the
  // auto-refetch schedule where transient failures during e.g. a WiFi
  // scan window are expected and resolve on the next attempt).
  refetch: (opts?: { silent?: boolean }) => void;
  // Client-side pending-action set. Keyed by action id. Useful for
  // indicators that must show BEFORE the first refetch lands
  // (e.g. the first 500 ms after Scan is clicked).
  isPending: (actionId: string) => boolean;
  markPending: (actionId: string, durationMs: number) => void;
  clearPending: (actionId: string) => void;
  // Current form-scoped state map (label→value). Consumers that render
  // decorations dependent on sibling-field values (ActionField busyIf,
  // TableField loadingIf) read from here instead of plumbing formState
  // through every DynamicContentHandler prop.
  formState: Record<string, any>;

  // Live (uncommitted) field values, keyed by label. Populated by
  // DynamicContentHandler on every handleInputChange — typing into a
  // TextField, rowFill from a TableField click, etc. Reflects the
  // operator's CURRENT intent before any Save/Action POST. ActionField
  // reads this first for `withFields` interpolation: the server-side
  // formState is a stale snapshot of the last GET, while the operator
  // may have just clicked a row half a second ago. Without this,
  // `?topic=` ships empty because formState['selected_topic'] is "".
  // Implemented as a getter (backed by a MutableRef) so per-keystroke
  // edits don't trigger context re-renders.
  getLiveValue?: (label: string) => any;
  setLiveValue?: (label: string, value: any) => void;
}

const noop = () => {};

export const DynamicFormContext = createContext<DynamicFormCtx>({
  refetch: noop,
  isPending: () => false,
  markPending: noop,
  clearPending: noop,
  formState: {},
  getLiveValue: () => undefined,
  setLiveValue: noop,
});

export const useDynamicForm = () => useContext(DynamicFormContext);
