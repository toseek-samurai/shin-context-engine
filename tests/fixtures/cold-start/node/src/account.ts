export interface AccountRecord {
  id: string;
}

export const loadAccount = async (id: string): Promise<AccountRecord> => ({ id });
