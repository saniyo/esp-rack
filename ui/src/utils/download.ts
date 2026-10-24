// Saving a file the device sent (fetched with the session's auth, so a bare
// link would not do).

// Content-Disposition: attachment; filename="x.elf" -> x.elf
export const fileNameOf = (cd?: string): string | undefined => {
  const m = cd ? /filename="?([^";]+)"?/i.exec(cd) : null;
  return m ? m[1] : undefined;
};

export const saveBlob = (blob: Blob, name: string) => {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  window.setTimeout(() => URL.revokeObjectURL(url), 1000);
};
