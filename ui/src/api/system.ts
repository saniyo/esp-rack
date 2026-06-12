import { AxiosPromise } from 'axios';
import { AXIOS, uploadFile, FileUploadConfig } from "./endpoints";
import { SystemStatus, OTASettings } from "../types";

export interface SystemInfo {
  platform: string;
  version: string;
}

export const readSystemStatus = (timeout?: number): AxiosPromise<SystemStatus> => {
  const config = timeout ? { timeout } : {};
  return AXIOS.get<SystemStatus>("system/status", config);
};

export const restart = (): AxiosPromise<void> => {
  return AXIOS.post<void>("actions/system.restart");
};

export const factoryReset = (): AxiosPromise<void> => {
  return AXIOS.post<void>("actions/system.factoryReset");
};

export const getSystemInfo = (): AxiosPromise<SystemInfo> => {
  return AXIOS.get<SystemInfo>("/api/info", { baseURL: "" });
};

export const readOTASettings = (): AxiosPromise<OTASettings> => {
  return AXIOS.get<OTASettings>("ota/form");
};

export const updateOTASettings = (otaSettings: OTASettings): AxiosPromise<OTASettings> => {
  return AXIOS.post<OTASettings>("ota/form", otaSettings);
};

export const uploadFirmware = (file: File, config?: FileUploadConfig): AxiosPromise<void> => {
  return uploadFile("/api/v1/system/upload/firmware", file, config);
};
