export type AuthLevel = 'public' | 'authenticated' | 'admin';

export interface MenuMeta {
  label?: string;
  icon?: string;
  order?: number;
  auth?: AuthLevel;
  hidden?: boolean;
}

export interface TabSpec {
  key: string;
  title?: string;
  restPath: string;
  postable?: boolean;  // tab can POST updates to restPath
  live?: boolean;       // tab subscribes to feature.ws for push updates
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
  ws?: string;
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
  device?: { name?: string; version?: string; frameworkVersion?: string };
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
}
