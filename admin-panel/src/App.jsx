import React, { useState, useEffect } from 'react';
import { supabase } from './supabaseClient';
import CreatableSelect from 'react-select/creatable';
import { Trash2, Plus, LogOut } from 'lucide-react';

export default function App() {
  const [session, setSession] = useState(null);

  useEffect(() => {
    supabase.auth.getSession().then(({ data: { session } }) => {
      setSession(session);
    });

    const { data: { subscription } } = supabase.auth.onAuthStateChange((_event, session) => {
      setSession(session);
    });

    return () => subscription.unsubscribe();
  }, []);

  if (!session) {
    return <Login />;
  }

  return <Dashboard key={session.user.id} userId={session.user.id} />;
}

// --- 1. LOGIN COMPONENT ---
function Login() {
  const handleLogin = async (provider) => {
    await supabase.auth.signInWithOAuth({ provider });
  };

  return (
    <div className="flex h-screen items-center justify-center bg-gray-100">
      <div className="bg-white p-8 rounded shadow-md w-96 text-center">
        <h1 className="text-2xl font-bold mb-6">Admin Sign In</h1>
        <div className="space-y-4">
          <button 
            onClick={() => handleLogin('google')}
            className="w-full bg-red-500 text-white py-2 rounded hover:bg-red-600 transition"
          >
            Sign in with Google
          </button>
          <button 
            onClick={() => handleLogin('github')}
            className="w-full bg-gray-800 text-white py-2 rounded hover:bg-gray-900 transition"
          >
            Sign in with GitHub
          </button>
        </div>
      </div>
    </div>
  );
}

// --- 2. DASHBOARD COMPONENT ---
function Dashboard({ userId }) {
  const [events, setEvents] = useState([]);
  const [view, setView] = useState('list'); // 'list' or 'edit'
  const [activeEventId, setActiveEventId] = useState(null);

  useEffect(() => {
    fetchEvents();
  }, []);

  const fetchEvents = async () => {
    const { data } = await supabase.from('events').select('*').order('created_at', { ascending: false });
    setEvents(data || []);
  };

  const createEvent = async () => {
    const { data, error } = await supabase
      .from('events')
      .insert([{ user_id: userId, name: 'New Event' }])
      .select()
      .single();
    
    if (data) {
      setEvents([data, ...events]);
      editEvent(data.id);
    }
  };

  const deleteEvent = async (id) => {
    if(!confirm("Are you sure?")) return;
    await supabase.from('events').delete().eq('id', id);
    setEvents(events.filter(e => e.id !== id));
  };

  const editEvent = (id) => {
    setActiveEventId(id);
    setView('edit');
  };

  const goBack = () => {
    setView('list');
    setActiveEventId(null);
    fetchEvents(); // refresh list
  };

  return (
    <div className="min-h-screen bg-gray-50 p-6">
      <div className="max-w-4xl mx-auto">
        <div className="flex justify-between items-center mb-8">
          <h1 className="text-3xl font-bold text-gray-800">Event Manager</h1>
          <button onClick={() => supabase.auth.signOut()} className="flex items-center text-gray-600 hover:text-red-500">
            <LogOut size={18} className="mr-2"/> Sign Out
          </button>
        </div>

        {view === 'list' ? (
          <div>
            <button 
              onClick={createEvent}
              className="mb-4 bg-blue-600 text-white px-4 py-2 rounded flex items-center hover:bg-blue-700"
            >
              <Plus size={18} className="mr-2"/> Create Event
            </button>
            <div className="grid gap-4">
              {events.map(event => (
                <div key={event.id} className="bg-white p-4 rounded shadow flex justify-between items-center">
                  <div>
                    <h3 className="font-semibold text-lg">{event.name}</h3>
                    <p className="text-gray-500 text-sm">ID: {event.id}</p>
                  </div>
                  <div className="space-x-2">
                    <button onClick={() => editEvent(event.id)} className="text-blue-600 hover:underline">Manage</button>
                    <button onClick={() => deleteEvent(event.id)} className="text-red-500 hover:text-red-700">
                      <Trash2 size={18}/>
                    </button>
                  </div>
                </div>
              ))}
            </div>
          </div>
        ) : (
          <EventEditor eventId={activeEventId} onBack={goBack} />
        )}
      </div>
    </div>
  );
}

// --- 3. EVENT EDITOR (Handles Buckets & Interests) ---
function EventEditor({ eventId, onBack }) {
  const [eventName, setEventName] = useState('');
  const [buckets, setBuckets] = useState([]);
  const [globalOptions, setGlobalOptions] = useState([]);

  useEffect(() => {
    loadEventDetails();
    loadGlobalInterests();
  }, [eventId]);

  const loadEventDetails = async () => {
    // 1. Get Event Info
    const { data: event } = await supabase.from('events').select('name').eq('id', eventId).single();
    if(event) setEventName(event.name);

    // 2. Get Buckets for this event
    const { data: bucketData } = await supabase
      .from('event_buckets')
      .select(`
        id, 
        name, 
        bucket_interests (
          interest:global_interests (id, name)
        )
      `)
      .eq('event_id', eventId)
      .order('created_at');

    // Transform data for UI
    const formattedBuckets = bucketData.map(b => ({
      id: b.id,
      name: b.name,
      interests: b.bucket_interests.map(bi => ({
        value: bi.interest.id,
        label: bi.interest.name
      }))
    }));

    setBuckets(formattedBuckets);
  };

  const loadGlobalInterests = async () => {
    const { data } = await supabase.from('global_interests').select('id, name');
    setGlobalOptions(data.map(i => ({ value: i.id, label: i.name })));
  };

  // --- ACTIONS ---

  const handleUpdateName = async () => {
    await supabase.from('events').update({ name: eventName }).eq('id', eventId);
  };

  const addBucket = async () => {
    const name = prompt("Enter Bucket Name (e.g. Languages, Domain)");
    if (!name) return;
    
    const { data } = await supabase.from('event_buckets').insert([{ event_id: eventId, name }]).select().single();
    if (data) setBuckets([...buckets, { id: data.id, name: data.name, interests: [] }]);
  };

  const deleteBucket = async (bucketId) => {
    if (!confirm("Delete bucket?")) return;
    await supabase.from('event_buckets').delete().eq('id', bucketId);
    setBuckets(buckets.filter(b => b.id !== bucketId));
  };

  // The Magic: Handle Select or Create Interest
  const handleInterestChange = async (bucketId, newValue, actionMeta) => {
    const currentBucket = buckets.find(b => b.id === bucketId);
    
    // 1. REMOVE Interest
    if (actionMeta.action === 'remove-value') {
      const removedId = actionMeta.removedValue.value;
      await supabase.from('bucket_interests')
        .delete()
        .match({ bucket_id: bucketId, interest_id: removedId });
      
      const updatedBuckets = buckets.map(b => 
        b.id === bucketId ? { ...b, interests: newValue } : b
      );
      setBuckets(updatedBuckets);
      return;
    }

    // 2. ADD Interest (Existing or New)
    const lastAdded = newValue[newValue.length - 1]; // The item just selected
    let interestId = lastAdded.value;

    // If it's a new custom tag (created via UI), insert into global first
    if (lastAdded.__isNew__) {
      const { data: newInterest, error } = await supabase
        .from('global_interests')
        .insert([{ name: lastAdded.label }])
        .select()
        .single();
      
      if (error) {
        alert("Error creating interest (might be duplicate)");
        return;
      }
      interestId = newInterest.id;
      
      // Update global options locally
      setGlobalOptions([...globalOptions, { value: newInterest.id, label: newInterest.name }]);
    }

    // Link to Bucket
    const { error: linkError } = await supabase
      .from('bucket_interests')
      .insert([{ bucket_id: bucketId, interest_id: interestId }]);

    if (!linkError) {
      // Update UI State
      const finalInterests = newValue.map(item => {
        if(item === lastAdded) return { value: interestId, label: item.label };
        return item;
      });

      const updatedBuckets = buckets.map(b => 
        b.id === bucketId ? { ...b, interests: finalInterests } : b
      );
      setBuckets(updatedBuckets);
    }
  };

  return (
    <div className="bg-white p-6 rounded shadow">
      <button onClick={onBack} className="text-gray-500 mb-4 hover:underline">&larr; Back to Dashboard</button>
      
      <div className="mb-6">
        <label className="block text-sm font-bold text-gray-700">Event Name</label>
        <input 
          type="text" 
          value={eventName} 
          onChange={(e) => setEventName(e.target.value)}
          onBlur={handleUpdateName}
          className="w-full mt-1 p-2 border rounded font-lg"
        />
      </div>

      <div className="border-t pt-6">
        <div className="flex justify-between items-center mb-4">
          <h2 className="text-xl font-bold">Interest Buckets</h2>
          <button onClick={addBucket} className="bg-green-600 text-white px-3 py-1 rounded text-sm hover:bg-green-700">
            + Add Bucket
          </button>
        </div>

        <div className="grid gap-6">
          {buckets.map(bucket => (
            <div key={bucket.id} className="bg-gray-50 p-4 rounded border relative">
              <div className="flex justify-between mb-2">
                <span className="font-bold text-gray-700 uppercase tracking-wide text-xs">{bucket.name}</span>
                <button onClick={() => deleteBucket(bucket.id)} className="text-red-400 hover:text-red-600">
                   <Trash2 size={16} />
                </button>
              </div>
              
              <CreatableSelect
                isMulti
                options={globalOptions}
                value={bucket.interests}
                onChange={(newValue, actionMeta) => handleInterestChange(bucket.id, newValue, actionMeta)}
                placeholder="Select or type to create..."
                className="basic-multi-select"
                classNamePrefix="select"
              />
            </div>
          ))}
          {buckets.length === 0 && <p className="text-gray-400 italic">No buckets yet.</p>}
        </div>
      </div>
    </div>
  );
}