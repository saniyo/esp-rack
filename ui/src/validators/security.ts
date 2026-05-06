import Schema, { InternalRuleItem } from "async-validator";

import { User } from "../types";

export const SECURITY_SETTINGS_VALIDATOR = new Schema({
  jwt_secret: [
    { required: true, message: "JWT secret is required" },
    { type: "string", min: 1, max: 64, message: "JWT secret must be between 1 and 64 characters" }
  ]
});

// Username uniqueness only matters when creating a new user — when
// editing, the username field is read-only and we don't want the
// existing row to fail its own uniqueness check.
//
// Password is required only when creating. On edit, leaving it blank
// signals the backend to preserve the existing PBKDF2 hash.
export const createUserValidator = (users: User[], creating: boolean) => new Schema({
  username: [
    { required: true, message: "Username is required" },
    { type: "string", pattern: /^[a-zA-Z0-9_.]{1,24}$/, message: "Must be 1-24 characters: alpha numeric, '_' or '.'" },
    ...(creating ? [createUniqueUsernameValidator(users)] : [])
  ],
  pwd: creating
    ? [
        { required: true, message: "Please provide a password" },
        { type: "string", min: 1, max: 64, message: "Password must be 1-64 characters" }
      ]
    : [
        { type: "string", max: 64, message: "Password must be at most 64 characters" }
      ],
});

export const createUniqueUsernameValidator = (users: User[]) => ({
  validator(rule: InternalRuleItem, username: string, callback: (error?: string) => void) {
    if (username && users.find((u) => u.username === username)) {
      callback("Username already in use");
    } else {
      callback();
    }
  }
});
