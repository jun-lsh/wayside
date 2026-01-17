import { createClient } from '@supabase/supabase-js';

const SUPABASE_URL = 'https://kqicvofhhghpsknsgaid.supabase.co';
const SUPABASE_ANON_KEY = 'sb_publishable_1OBjewg0cWpse0iABrgJSg_RZmHjPLk';

export const supabase = createClient(SUPABASE_URL, SUPABASE_ANON_KEY);