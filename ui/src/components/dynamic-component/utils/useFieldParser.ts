// useFieldParser.ts
import { useMemo } from 'react';
import { parseFieldOptions, ParsedFieldOptions } from './fieldParser';

export const useFieldParser = (label: string, optionsString: string): ParsedFieldOptions => {
  return useMemo(() => {
    return parseFieldOptions(label, optionsString);
  }, [label, optionsString]);
};
