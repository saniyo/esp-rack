import { FC } from 'react';

import DynamicFeature from '../../components/dynamic-component/DynamicFeature';

// Thin wrapper — the real page is rendered from the 'system' compound
// feature in /api/v1/ui/manifest, which declares three tabs: status / ota /
// upload. Each tab fetches its own restPath (/api/v1/system/status/form,
// /api/v1/ota/form, /api/v1/system/upload/form) and renders a
// FormBuilder-produced form.
const System: FC = () => <DynamicFeature featureId="system" />;

export default System;
