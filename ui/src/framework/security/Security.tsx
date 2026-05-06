import { FC } from 'react';

import { useLayoutTitle } from '../../components';

import ManageUsersForm from './ManageUsersForm';

// Single-page Security feature — User CRUD only. The JWT signing key
// lives on the System → JWT tab now (separate concern: it's an
// admin-only signing secret, not a directory of accounts).
const Security: FC = () => {
  useLayoutTitle("Security");
  return <ManageUsersForm />;
};

export default Security;
