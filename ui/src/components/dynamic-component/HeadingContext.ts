// the titles a page already shows (feature, menu entry, tab), so
// forms and cards inside don't repeat them.
import React from 'react';

export const HeadingContext = React.createContext<(string | undefined)[]>([]);
