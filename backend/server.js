const express = require('express');
const cors    = require('cors');

const app = express();
app.use(cors());
app.use(express.json());

// ── In-memory agent registry ──────────────────────────────────────────────────
const devices = new Map(); // deviceId -> device record

const ONLINE_THRESHOLD_MS = 20000; // 20 seconds

function getActiveDevices() {
  const now  = Date.now();
  const list = [];
  for (const [, dev] of devices.entries()) {
    const lastSeen = dev.lastHeartbeat || 0;
    list.push({
      deviceId:    dev.deviceId,
      hostname:    dev.hostname,
      username:    dev.username,
      localIp:     dev.localIp,
      publicIp:    dev.publicIp,
      status:      (now - lastSeen) < ONLINE_THRESHOLD_MS ? 'online' : 'offline',
      lastSeenAgo: Math.round((now - lastSeen) / 1000) + 's'
    });
  }
  return list;
}

// ── GET /api/health ───────────────────────────────────────────────────────────
app.get('/api/health', (req, res) => {
  res.json({ status: 'ok', time: new Date().toISOString() });
});

// ── POST /api/heartbeat  (Agent calls this every 5-10s) ──────────────────────
app.post('/api/heartbeat', (req, res) => {
  const { deviceId, hostname, username, localIp, publicIp } = req.body;
  if (!deviceId) return res.status(400).json({ error: 'deviceId is required' });

  const existing = devices.get(deviceId) || {};
  devices.set(deviceId, {
    deviceId,
    hostname:      hostname  || existing.hostname  || 'Unknown',
    username:      username  || existing.username  || 'system',
    localIp:       localIp   || existing.localIp   || '0.0.0.0',
    publicIp:      publicIp  || existing.publicIp  || req.ip || '0.0.0.0',
    lastHeartbeat: Date.now()
  });

  res.json({ success: true });
});

// ── GET /api/devices  (Monitor dashboard & controller list) ───────────────────
app.get('/api/devices', (req, res) => {
  res.json({ success: true, devices: getActiveDevices() });
});

// ── Catch-all: reject everything else ────────────────────────────────────────
app.use((req, res) => {
  res.status(404).json({ error: 'Not found' });
});

// ── Start server ──────────────────────────────────────────────────────────────
if (require.main === module || !process.env.VERCEL) {
  const PORT = process.env.PORT || 8000;
  app.listen(PORT, '0.0.0.0', () => {
    console.log(`TRG Backend running on port ${PORT}`);
    console.log(`Routes: POST /api/heartbeat | GET /api/devices | GET /api/health`);
  });
}

module.exports = app;
