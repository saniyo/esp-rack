import React from 'react';
import { Field } from '../types'; // Ensure the correct path to types

const DefaultComponent: React.FC<{ field: Field }> = ({ field }) => (
  <div style={{ border: '1px solid red', padding: '10px', margin: '10px 0' }}>
    <strong style={{ color: 'red' }}>Unsupported Field Type: {field.type}</strong>
    <pre style={{ whiteSpace: 'pre-wrap', wordWrap: 'break-word' }}>
      {JSON.stringify(field, null, 2)}
    </pre>
  </div>
);

export default DefaultComponent;
