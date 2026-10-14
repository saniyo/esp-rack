import { FC } from 'react';
import { CssBaseline } from '@mui/material';
import { createTheme, responsiveFontSizes, ThemeProvider } from '@mui/material/styles';
import { indigo, blueGrey, orange, red, green } from '@mui/material/colors';
import { RequiredChildrenProps } from './utils';

const theme = responsiveFontSizes(
  createTheme({
    palette: {
      background: {
        default: "#fafafa"
      },
      primary: indigo,
      secondary: blueGrey,
      info: {
        main: indigo[500]
      },
      warning: {
        main: orange[500]
      },
      error: {
        main: red[500]
      },
      success: {
        main: green[500]
      }
    },
    // SES-LIB-029: compact forms everywhere (one rule, not per form).
    components: {
      MuiTextField: { defaultProps: { size: 'small', margin: 'dense' } },
      MuiFormControl: { defaultProps: { size: 'small', margin: 'dense' } },
      MuiSelect: { defaultProps: { size: 'small' } },
      MuiListItem: { styleOverrides: { root: { paddingTop: 2, paddingBottom: 2 } } },
      MuiCardHeader: { styleOverrides: { root: { padding: '8px 12px' } } },
      MuiCardContent: { styleOverrides: { root: { padding: 12, '&:last-child': { paddingBottom: 12 } } } },
    },
  })
);

const CustomTheme: FC<RequiredChildrenProps> = ({ children }) => (
  <ThemeProvider theme={theme}>
    <CssBaseline />
    {children}
  </ThemeProvider>
);

export default CustomTheme;
