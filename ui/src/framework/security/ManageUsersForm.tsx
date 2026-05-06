import { FC, useContext, useState } from 'react';

import {
  Button, IconButton, Table, TableBody, TableCell, TableFooter, TableHead, TableRow
} from '@mui/material';
import SaveIcon from '@mui/icons-material/Save';
import DeleteIcon from '@mui/icons-material/Delete';
import PersonAddIcon from '@mui/icons-material/PersonAdd';
import EditIcon from '@mui/icons-material/Edit';
import CheckIcon from '@mui/icons-material/Check';
import CloseIcon from '@mui/icons-material/Close';

import * as SecurityApi from "../../api/security";
import { SecuritySettings, User } from '../../types';
import { ButtonRow, FormLoader, MessageBox, SectionContent } from '../../components';
import { createUserValidator } from '../../validators';
import { useRest } from '../../utils';
import { AuthenticatedContext } from '../../contexts/authentication';

import UserForm from './UserForm';

function compareUsers(a: User, b: User) {
  if (a.username < b.username) return -1;
  if (a.username > b.username) return 1;
  return 0;
}

const ManageUsersForm: FC = () => {
  const {
    loadData, saving, data, setData, saveData, errorMessage
  } = useRest<SecuritySettings>({
    read: SecurityApi.readSecuritySettings,
    update: SecurityApi.updateSecuritySettings,
  });

  const [user, setUser] = useState<User>();
  const [creating, setCreating] = useState<boolean>(false);
  const authenticatedContext = useContext(AuthenticatedContext);

  const content = () => {
    if (!data) {
      return <FormLoader onRetry={loadData} errorMessage={errorMessage} />;
    }

    const noAdminConfigured = () => !data.users.find((u) => u.admin);

    const removeUser = (toRemove: User) => {
      const users = data.users.filter((u) => u.username !== toRemove.username);
      setData({ ...data, users });
    };

    const createUser = () => {
      setCreating(true);
      setUser({
        username: "",
        pwd: "",
        admin: true,
      });
    };

    // On edit we deliberately blank the password field — the value the
    // backend served is the PBKDF2 hash, which is useless to display
    // and confusing to mask. Empty value on save tells the backend to
    // preserve the existing hash (see SecuritySettings::update). The
    // operator types a NEW password to change it, leaves blank to keep.
    const editUser = (toEdit: User) => {
      setCreating(false);
      setUser({ ...toEdit, pwd: "" });
    };

    const cancelEditingUser = () => {
      setUser(undefined);
    };

    // After the modal closes, splice the edited user back into the list.
    // For a NEW user (creating), the password field carried a fresh
    // plaintext value — it goes to the backend and gets hashed there.
    // For an EDIT, the password field is either "" (preserve hash) or
    // a fresh plaintext (rehash on backend).
    const doneEditingUser = () => {
      if (user) {
        const users = [...data.users.filter((u) => u.username !== user.username), user];
        setData({ ...data, users });
        setUser(undefined);
      }
    };

    const onSubmit = async () => {
      await saveData();
      authenticatedContext.refresh();
    };

    return (
      <>
        <Table size="small">
          <TableHead>
            <TableRow>
              <TableCell>Username</TableCell>
              <TableCell align="center">Admin?</TableCell>
              <TableCell />
            </TableRow>
          </TableHead>
          <TableBody>
            {data.users.sort(compareUsers).map((u) => (
              <TableRow key={u.username}>
                <TableCell component="th" scope="row">{u.username}</TableCell>
                <TableCell align="center">{u.admin ? <CheckIcon /> : <CloseIcon />}</TableCell>
                <TableCell align="center">
                  <IconButton size="small" aria-label="Delete" onClick={() => removeUser(u)}>
                    <DeleteIcon />
                  </IconButton>
                  <IconButton size="small" aria-label="Edit" onClick={() => editUser(u)}>
                    <EditIcon />
                  </IconButton>
                </TableCell>
              </TableRow>
            ))}
          </TableBody>
          <TableFooter>
            <TableRow>
              <TableCell colSpan={2} />
              <TableCell align="center" padding="normal">
                <Button startIcon={<PersonAddIcon />} variant="contained" color="secondary" onClick={createUser}>
                  Add
                </Button>
              </TableCell>
            </TableRow>
          </TableFooter>
        </Table>
        {noAdminConfigured() && (
          <MessageBox
            level="warning"
            message="You must have at least one admin user configured."
            my={2}
          />
        )}
        <ButtonRow mt={2}>
          <Button
            startIcon={<SaveIcon />}
            disabled={saving || noAdminConfigured()}
            variant="contained"
            color="primary"
            type="submit"
            onClick={onSubmit}
          >
            Save
          </Button>
        </ButtonRow>
        <UserForm
          user={user}
          setUser={setUser}
          creating={creating}
          onDoneEditing={doneEditingUser}
          onCancelEditing={cancelEditingUser}
          validator={createUserValidator(data.users, creating)}
        />
      </>
    );
  };

  return (
    <SectionContent title='Manage Users' titleGutter>
      {content()}
    </SectionContent>
  );
};

export default ManageUsersForm;
