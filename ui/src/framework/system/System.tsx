import { FC } from 'react';

import DynamicFeature from '../../components/dynamic-component/DynamicFeature';

// Thin wrapper — the real page is rendered from the 'system' compound
// feature in /rest/uiManifest, which declares three tabs: status / ota /
// upload. Each tab fetches its own restPath (SYSTEM_STATUS_FORM_PATH,
// OTA_SETTINGS_FORM_PATH, UPLOAD_FIRMWARE_FORM_PATH) and renders a
// FormBuilder-produced form.
const System: FC = () => <DynamicFeature featureId="system" />;

export default System;
