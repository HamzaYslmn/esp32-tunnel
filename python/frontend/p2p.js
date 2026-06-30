// esp32-tunnel P2P client — WebRTC DataChannel, automatic relay fallback.
//   const dev = espTunnel('https://server', 'device');
//   const res = await dev.fetch('/status');   // res.via: 'p2p' | 'relay'
//
// DataChannel wire protocol (device must mirror):
//   browser -> device: {"i":id,"m":method,"p":path,"b":body}
//   device  -> browser: {"i":id,"s":status,"b":body,"t":contentType}
//
// ponytail: non-trickle ICE (simpler); STUN only, no TURN -> symmetric NAT uses relay.

function espTunnel(server, id, { stun = 'stun:stun.l.google.com:19302', key = '' } = {}) {
  let chan = null, connecting = null, declined = false;   // declined: device has no P2P engine
  const waiters = new Map();
  let seq = 0;
  const auth = key ? { 'X-Tunnel-Key': key } : {};   // device access key

  async function connect() {
    const pc = new RTCPeerConnection({ iceServers: [{ urls: stun }] });
    const dc = pc.createDataChannel('t');
    dc.onmessage = (e) => {
      const m = JSON.parse(e.data);
      const w = waiters.get(m.i);
      if (w) { waiters.delete(m.i); w({ status: m.s, body: m.b ?? '', type: m.t || '' }); }
    };
    pc.oniceconnectionstatechange = () => {
      if (['failed', 'disconnected', 'closed'].includes(pc.iceConnectionState)) chan = null;
    };

    await pc.setLocalDescription(await pc.createOffer());
    await new Promise((r) => {                 // wait for full ICE gather (non-trickle)
      if (pc.iceGatheringState === 'complete') return r();
      pc.onicegatheringstatechange = () => pc.iceGatheringState === 'complete' && r();
    });

    const res = await fetch(`${server}/api/tunnel/${id}/_signal`, {
      method: 'POST', body: pc.localDescription.sdp, headers: auth,
    });
    // 501 = device has no P2P engine: remember it so we stop re-attempting and
    // go straight to relay (one round-trip instead of handshake + relay).
    if (res.status === 501) { declined = true; pc.close(); throw new Error('no p2p'); }
    if (!res.ok) throw new Error('signal ' + res.status);
    await pc.setRemoteDescription({ type: 'answer', sdp: await res.text() });

    await new Promise((ok, no) => {
      const t = setTimeout(() => no(new Error('dc timeout')), 12000);
      dc.onopen = () => { clearTimeout(t); ok(); };
    });
    chan = dc;
  }

  async function ensure() {
    if (declined) return false;                          // known no-P2P device -> relay directly
    if (chan && chan.readyState === 'open') return true;
    if (!connecting) connecting = connect().finally(() => { connecting = null; });
    try { await connecting; return true; } catch { return false; }
  }

  async function relay(path, opts = {}) {
    const r = await fetch(`${server}/${id}${path.startsWith('/') ? '' : '/'}${path}`,
      { ...opts, headers: { ...auth, ...opts.headers } });
    return { status: r.status, body: await r.text(), type: r.headers.get('content-type') || '', via: 'relay' };
  }

  return {
    async fetch(path, opts = {}) {
      if (!(await ensure())) return relay(path, opts);
      const i = ++seq;
      const p = new Promise((res) => waiters.set(i, res));
      chan.send(JSON.stringify({ i, m: opts.method || 'GET', p: path, b: opts.body || '' }));
      const done = await Promise.race([p, new Promise((r) => setTimeout(() => r(null), 30000))]);
      if (!done) { waiters.delete(i); return relay(path, opts); }  // P2P stalled -> relay
      return { ...done, via: 'p2p' };
    },
  };
}

if (typeof window !== 'undefined') window.espTunnel = espTunnel;
