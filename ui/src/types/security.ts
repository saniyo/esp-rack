export interface User {
  username: string;
  // Round-trips as the PBKDF2 hex hash + salt the backend served.
  // The UI never displays it; an empty value on save signals
  // "keep existing hash" to the server's update() pipeline.
  pwd: string;
  salt?: string;
  admin: boolean;
}

export interface SecuritySettings {
  users: User[];
  jwt_secret: string;
}
