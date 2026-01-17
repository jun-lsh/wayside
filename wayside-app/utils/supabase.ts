import 'react-native-url-polyfill/auto';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { createClient } from '@supabase/supabase-js';

// no env vars we hardcode URLs like my sanity at 3am
const SUPABASE_URL = 'https://kqicvofhhghpsknsgaid.supabase.co';
const SUPABASE_ANON_KEY = 'sb_publishable_1OBjewg0cWpse0iABrgJSg_RZmHjPLk';

export const supabase = createClient(SUPABASE_URL, SUPABASE_ANON_KEY, {
    auth: {
      storage: AsyncStorage,
      autoRefreshToken: true,
      persistSession: true,
      detectSessionInUrl: false,
    },
  });