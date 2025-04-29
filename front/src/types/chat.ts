export interface ChatMessage {
  uuid: string;
  username: string;
  message: string;
  timestamp: number;
}

export interface User {
  uuid: string;
  username: string;
}