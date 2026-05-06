export interface Field {
  label: string;
  value: any;
  o: string;
  type?: string;
  placeholder?: string;
  readOnly?: boolean;
  options?: string[];
  min?: number;
  max?: number;
  step?: number;
  icon?: string;  // MUI icon name resolved via iconRegistry
}

export interface Form {
  title: string;
  description: string;
  fields: Field[];
}

export interface Data {
  form: Form;
  [key: string]: Form | any;
}

export interface TrendChartProps {
  value: any[];
  dataKeys: string[];
}
