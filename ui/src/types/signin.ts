export interface SignInRequest {
  username: string;
  pwd: string;
}

export interface SignInResponse {
  access_token: string;
}
