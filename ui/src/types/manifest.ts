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

export interface UiManifest {
  schemaVersion: number;
  device?: { name?: string; version?: string };
  buildFeatures?: Record<string, boolean>;
  features: FeatureEntry[];
}
