// MUI icon registry for dynamic widgets.
//
// Keys match the exact file name under @mui/icons-material — so
// icon("Thermostat") from C++ resolves to ThermostatIcon and it's
// transparent: no C++/frontend name drift, no full paths, no uploads.
//
// Bundle policy — curated list, tree-shaken by webpack. Adding a new icon
// is one import + one map entry. Don't use `import * as Icons` (1.5 MB gzip).
//
// If an icon name isn't in the map, resolveIcon returns null and callers
// fall back to their default (typically UpdateIcon).

import React, { FC } from 'react';
import { Avatar } from '@mui/material';
import { styled } from '@mui/material/styles';
import { SvgIconComponent } from '@mui/icons-material';

// ---- starter set — names match @mui/icons-material paths exactly ----
import Update from '@mui/icons-material/Update';
import Thermostat from '@mui/icons-material/Thermostat';
import Lightbulb from '@mui/icons-material/Lightbulb';
import Bolt from '@mui/icons-material/Bolt';
import BatteryFull from '@mui/icons-material/BatteryFull';
import Speed from '@mui/icons-material/Speed';
import Settings from '@mui/icons-material/Settings';
import Folder from '@mui/icons-material/Folder';
import Wifi from '@mui/icons-material/Wifi';
import Cloud from '@mui/icons-material/Cloud';
import Storage from '@mui/icons-material/Storage';
import Lock from '@mui/icons-material/Lock';
import LockOpen from '@mui/icons-material/LockOpen';
import Visibility from '@mui/icons-material/Visibility';
import Info from '@mui/icons-material/Info';
import Warning from '@mui/icons-material/Warning';
import ErrorIcon from '@mui/icons-material/Error';
import Help from '@mui/icons-material/Help';
import PlayArrow from '@mui/icons-material/PlayArrow';
import Pause from '@mui/icons-material/Pause';
import Stop from '@mui/icons-material/Stop';
import PowerSettingsNew from '@mui/icons-material/PowerSettingsNew';
import Refresh from '@mui/icons-material/Refresh';
import Save from '@mui/icons-material/Save';
import Delete from '@mui/icons-material/Delete';
import Edit from '@mui/icons-material/Edit';
import Home from '@mui/icons-material/Home';
import Dashboard from '@mui/icons-material/Dashboard';
import Assessment from '@mui/icons-material/Assessment';
import Timeline from '@mui/icons-material/Timeline';
import TableChart from '@mui/icons-material/TableChart';
import Notifications from '@mui/icons-material/Notifications';
import Sensors from '@mui/icons-material/Sensors';
import Tune from '@mui/icons-material/Tune';
import ToggleOn from '@mui/icons-material/ToggleOn';
import CheckBox from '@mui/icons-material/CheckBox';
import RadioButtonChecked from '@mui/icons-material/RadioButtonChecked';
import ArrowDropDown from '@mui/icons-material/ArrowDropDown';
import TextFields from '@mui/icons-material/TextFields';
import Notes from '@mui/icons-material/Notes';
import Upload from '@mui/icons-material/Upload';
import ShowChart from '@mui/icons-material/ShowChart';
import BarChart from '@mui/icons-material/BarChart';
import PieChart from '@mui/icons-material/PieChart';
import Memory from '@mui/icons-material/Memory';
import Timer from '@mui/icons-material/Timer';
import Devices from '@mui/icons-material/Devices';
import Apps from '@mui/icons-material/Apps';
import DataUsage from '@mui/icons-material/DataUsage';
import SdStorage from '@mui/icons-material/SdStorage';
import Telegram from '@mui/icons-material/Telegram';
import Send from '@mui/icons-material/Send';
import DeviceHub from '@mui/icons-material/DeviceHub';
import Topic from '@mui/icons-material/Topic';
import Sync from '@mui/icons-material/Sync';
import Dns from '@mui/icons-material/Dns';
import SwapVerticalCircle from '@mui/icons-material/SwapVerticalCircle';
import AvTimer from '@mui/icons-material/AvTimer';
import SettingsInputAntenna from '@mui/icons-material/SettingsInputAntenna';
import Computer from '@mui/icons-material/Computer';
import Router from '@mui/icons-material/Router';
import Language from '@mui/icons-material/Language';
import Schedule from '@mui/icons-material/Schedule';
import CalendarToday from '@mui/icons-material/CalendarToday';
import AccessTime from '@mui/icons-material/AccessTime';
import SettingsBackupRestore from '@mui/icons-material/SettingsBackupRestore';
import NetworkPing from '@mui/icons-material/NetworkPing';

export const ICON_MAP: Record<string, SvgIconComponent> = {
  Update,
  Thermostat,
  Lightbulb,
  Bolt,
  BatteryFull,
  Speed,
  Settings,
  Folder,
  Wifi,
  Cloud,
  Storage,
  Lock,
  LockOpen,
  Visibility,
  Info,
  Warning,
  Error: ErrorIcon,
  Help,
  PlayArrow,
  Pause,
  Stop,
  PowerSettingsNew,
  Refresh,
  Save,
  Delete,
  Edit,
  Home,
  Dashboard,
  Assessment,
  Timeline,
  TableChart,
  Notifications,
  Sensors,
  Tune,
  ToggleOn,
  CheckBox,
  RadioButtonChecked,
  ArrowDropDown,
  TextFields,
  Notes,
  Upload,
  ShowChart,
  BarChart,
  PieChart,
  Memory,
  Timer,
  Schedule,
  CalendarToday,
  AccessTime,
  SettingsBackupRestore,
  NetworkPing,
  Devices,
  Apps,
  DataUsage,
  SdStorage,
  Telegram,
  Send,
  DeviceHub,
  Topic,
  Sync,
  Dns,
  SwapVerticalCircle,
  AvTimer,
  SettingsInputAntenna,
  Computer,
  Router,
  Language,
};

export function resolveIcon(name?: string | null): SvgIconComponent | null {
  if (!name) return null;
  return ICON_MAP[name] ?? null;
}

// ---- FieldAvatar: unified avatar used by every dynamic element.
// Reads icon name from the field's parsed options (icon=Foo in field.o) and
// resolves from ICON_MAP. Falls back to the default (usually Update) if the
// name is missing or not registered, so existing elements keep their current
// look when C++ doesn't set an icon. Background color comes from the theme
// palette key passed via the `color=<name>` tag on the backend (primary |
// secondary | success | error | warning | info). Default = primary.
export type AvatarColorName = 'primary' | 'secondary' | 'success' | 'error' | 'warning' | 'info';

const AVATAR_COLOR_KEYS: ReadonlySet<string> =
  new Set(['primary', 'secondary', 'success', 'error', 'warning', 'info']);

interface StyledAvatarProps {
  colorName?: string;
}
const StyledAvatar = styled(Avatar, {
  shouldForwardProp: (p) => p !== 'colorName',
})<StyledAvatarProps>(({ theme, colorName }) => {
  const key = colorName && AVATAR_COLOR_KEYS.has(colorName) ? colorName : 'primary';
  return {
    backgroundColor: (theme.palette as any)[key].main,
    borderRadius: '50%',
    width: theme.spacing(5),
    height: theme.spacing(5),
  };
});

interface FieldAvatarProps {
  iconName?: string | null;
  fallback?: SvgIconComponent;
  colorName?: string | null;
}

export const FieldAvatar: FC<FieldAvatarProps> = ({ iconName, fallback, colorName }) => {
  const Resolved = resolveIcon(iconName) ?? fallback ?? Update;
  return (
    <StyledAvatar colorName={colorName || undefined}>
      <Resolved sx={{ color: 'common.white' }} />
    </StyledAvatar>
  );
};
