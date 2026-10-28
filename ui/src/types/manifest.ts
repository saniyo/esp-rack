export type AuthLevel = 'public' | 'authenticated' | 'admin';

export interface MenuMeta {
  label?: string;
  icon?: string;
  order?: number;
  auth?: AuthLevel;
  hidden?: boolean;
  // Tree-menu hierarchy (additive; both absent = legacy flat top-level).
  // parent: id of the feature/group this item nests UNDER; undefined =
  // top-level. The backend is the single source of truth for placement.
  parent?: string;
  // hw: this page belongs to external HW instance <hw>. Its
  // placement comes from UiManifest.extHw (components/layout/extHwMenu.ts),
  // never from parent.
  hw?: string;
  // expandTabs: also surface this feature's own tabs as leaf children in
  // the tree (jump straight to /<id>/<tabKey>), beside any child features.
  expandTabs?: boolean;
  // role: the device's MAIN-ROLE feature (the consumer app). In the
  // device discovery menu, role features render directly under the device record;
  // everything else goes into the "Device" group. Absent/false = infra.
  role?: boolean;
  // landing: the page the UI opens first (else the role feature, else the
  // first item).
  landing?: boolean;
}

export interface TabSpec {
  key: string;
  title?: string;
  icon?: string;        // optional per-tab MUI icon, surfaced on the menu sub-tab leaf
  restPath: string;
  // a bespoke page tab with a static schema: the UI caches
  // <restPath>?schema=1 and takes the values from this flat GET path.
  statePath?: string;
  // A framework renderer for this tab instead of a form (CUSTOM_TABS).
  component?: string;
  postable?: boolean;  // tab can POST updates to restPath
  live?: boolean;       // tab subscribes to feature.ch over the shared /ws/app socket
  auth?: AuthLevel;
}

export interface ActionSpec {
  id: string;
  title?: string;
  icon?: string;
  restPath: string;
  auth?: AuthLevel;
  confirm?: boolean;
  successMessage?: string;
  download?: string;  // the response is a file to save (default name)
}

export interface EndpointMeta {
  method: 'GET' | 'POST' | 'PUT' | 'DELETE' | 'PATCH';
  path: string;
  auth?: AuthLevel;
  role?: string;
}

export interface FeatureEntry {
  id: string;
  kind: 'feature' | 'action' | 'status' | 'page';
  title?: string;
  // Service-implementation version (semver). Optional — services that
  // don't set it just don't report it. Distinct from the wrapping
  // module's own version which lives in `UiManifest.modules[]`.
  version?: string;
  component?: string;
  route?: string | null;
  menu?: MenuMeta;
  auth?: AuthLevel;
  rest?: { read?: string; update?: string };
  // Live features advertise a CHANNEL id, multiplexed over the single
  // /ws/app socket (see WsProvider). `ws` is the legacy per-feature socket
  // path, kept optional for back-compat; current backends emit `ch`.
  ch?: string;
  ws?: string;
  // ss: the form's fields don't depend on the state: the UI
  // caches <form>?schema=1 and fills it from /state.
  ss?: number;
  tabs?: TabSpec[];
  actions?: ActionSpec[];
  endpoints?: EndpointMeta[];
}

export interface ModuleEntry {
  id: string;
  version: string;
}

export interface UiManifest {
  schemaVersion: number;
  // device.name / version  — consumer-app identity from Builder("...","...")
  // device.frameworkVersion — esp-rack library rev (Version.h::ESPRACK_VERSION_STR)
  // device.tz — the device's timezone: IANA label and POSIX rule.
  //             Device-local times are formatted in it (utils/deviceTime.ts).
  device?: { name?: string; version?: string; frameworkVersion?: string; tz?: { iana?: string; posix?: string } };
  buildFeatures?: Record<string, boolean>;
  // True when the request carrying this manifest had a valid JWT.
  // false on anonymous (stub) responses — modules[] and features[]
  // are absent. SignIn page handles `false` by rendering with the
  // brand-only data; ManifestLoader re-fetches after sign-in to swap
  // stub for full content.
  authenticated?: boolean;
  // Module wrapper rev list, one entry per Module installed via Builder.
  // Populated by App.begin() walking modules_ and calling describe().
  // Omitted from the anonymous stub.
  modules?: ModuleEntry[];
  features: FeatureEntry[];
  // External HW instances, from the backend HwRegistry: the
  // UI's only source for where their pages go. Live state is in
  // System -> External Hardware.
  extHw?: ExtHwEntry[];
}

export interface ExtHwEntry {
  id: string;
  type: string;
  title?: string;
  owner?: string;        // the module (and menu entry) that uses it
  interfaces?: string[];
  pages?: string[];      // feature ids with menu.hw == id
}
