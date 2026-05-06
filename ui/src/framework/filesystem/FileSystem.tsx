import { FC, useEffect, useState } from 'react';
import { SectionContent, useLayoutTitle } from '../../components';
import DynamicSettings from '../../components/dynamic-component/DynamicSettings';
import FormLoader from '../../components/loading/FormLoader';
import { fetchSchema } from '../../api/fs';

const FileSystem: FC = () => {
  useLayoutTitle('File System');

  const [data, setData] = useState<any>(null);
  const [errorMessage, setErrorMessage] = useState<string | undefined>();

  useEffect(() => {
    fetchSchema()
      .then((r) => setData(r.data))
      .catch((e) => setErrorMessage(e?.message || 'schema_failed'));
  }, []);

  if (!data) {
    return (
      <SectionContent title="File System" titleGutter>
        <FormLoader errorMessage={errorMessage} message="Loading file system..." />
      </SectionContent>
    );
  }

  const form = data.browser || { description: '', fields: [] };

  return (
    <DynamicSettings
      formName="browser"
      data={form}
      saveData={() => {}}
      saving={false}
      setData={() => {}}
    />
  );
};

export default FileSystem;
