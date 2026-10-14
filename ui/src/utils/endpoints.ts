import { AxiosError } from "axios";

// problem+json (RFC 9457) detail, else its title; the older {message} body;
// else axios' own text ("Request failed with status code 409").
export const extractErrorMessage = (
  error: AxiosError<{ message?: string; detail?: string; title?: string }>,
  defaultMessage: string,
) => {
  const d = error?.response?.data;
  return d?.detail || d?.title || d?.message || error?.message || defaultMessage;
};
