'use strict';
// Subscription / plan-quota tracker: live rate-limit and credit windows for
// whatever subscription plans the user wires up in the config (subs.providers).
//
// Adapters (each entry picks one by `type`):
//  - chatgpt: ChatGPT plan via a Codex CLI login.
//      GET https://chatgpt.com/backend-api/wham/usage
//        Bearer access_token from <authPath> (auth_mode "chatgpt").
//        Returns plan_type + primary (5h-ish) / secondary (weekly) windows.
//  - zai: Z.ai coding plan.
//      GET https://api.z.ai/api/monitor/usage/quota/limit
//        authorization = coding-plan apiKey read from <configPath>
//        (provider <provider>, default builtin:zai-coding-plan).
//        unit 3 = rolling hours, unit 6 = week.
//
// Snapshot shape (every provider, normalized):
//   { ok, label, plan, status, windows: [{ key, label, percent, used, total,
//     remaining, resetAt }], notes: [], errors: [], fetchedAt }
// The tracker snapshot is { enabled, providers: {id: snapshot}, lastScan }.
const fs = require('fs');
const path = require('path');
const os = require('os');
const { EventEmitter } = require('events');

function emptyProvider(label, errors) {
  return {
    ok: false,
    label,
    plan: null,
    status: 'unknown',
    windows: [],
    notes: [],
    errors: errors || [],
    fetchedAt: Date.now()
  };
}

function percentLabel(used, total) {
  if (!(total > 0)) return 0;
  return Math.min(100, Math.max(0, (used / total) * 100));
}

function windowLabel(unit, number) {
  // Consistent wording with ChatGPT: "5h" / "week".
  if (unit === 3) return `${number || 5}h`;
  if (unit === 6) return 'week';
  return unit === 2 ? 'day' : `unit${unit}`;
}

/** Remaining-available percent (100 = unused, 0 = exhausted). */
function remainingPercent(usedPct) {
  return Math.min(100, Math.max(0, 100 - usedPct));
}

/**
 * Provider status from ALL plan windows, using the LOWEST remaining.
 * A drained 5h window is capped even if week still has room.
 */
function statusFromWindows(windows) {
  if (!windows.length) return 'unknown';
  const minRem = windows.reduce((m, w) => Math.min(m, w.remainingPercent ?? 100), 100);
  if (minRem <= 0) return 'blocked';      // capped
  if (minRem <= 10) return 'critical';    // near cap
  return 'ok';
}

class SubsTracker extends EventEmitter {
  constructor(config, fetchImpl) {
    super();
    this.cfg = (config && config.subs) || {};
    // Injectable for tests; production uses global fetch.
    this._fetch = fetchImpl || ((u, o) => fetch(u, o));
    // Last good windows per provider id: a timed-out or erroring cycle keeps
    // the previous windows on the board (marked stale) instead of wiping it.
    this._lastGood = new Map();
    this.state = { enabled: !!(this.cfg && this.cfg.enabled), providers: {}, lastScan: null };
    this._timer = null;
    this._busy = null;
  }

  // Per-attempt request deadline. Clamped so a bad config can neither make
  // the board hammer the endpoint (too low) nor hang a scan for minutes (too
  // high). A provider entry may override it with its own timeoutMs, subject
  // to the same clamp.
  _timeoutFor(p) {
    const t = Number(
      (p && typeof p.timeoutMs === 'number' && p.timeoutMs > 0) ? p.timeoutMs : this.cfg.fetchTimeoutMs
    );
    return Number.isFinite(t) ? Math.min(60000, Math.max(3000, t)) : 20000;
  }

  start() {
    clearInterval(this._timer);
    if (!this.state.enabled) return; // master off: no polls, no requests
    const mins = Math.max(0.5, (this.cfg.intervalMinutes || 2));
    this._timer = setInterval(() => this.rescan(), mins * 60000);
    this.rescan();
  }

  setConfig(config) {
    this.cfg = (config && config.subs) || {};
    const enabled = !!(this.cfg && this.cfg.enabled);
    if (enabled !== this.state.enabled) {
      this.state = { enabled, providers: {}, lastScan: this.state.lastScan };
      this.emit('updated', this.state);
    }
    this.start();
  }

  stop() {
    clearInterval(this._timer);
    this._timer = null;
  }

  snapshot() {
    return this.state;
  }

  rescan() {
    if (!this.state.enabled) return Promise.resolve(this.state);
    if (this._busy) return this._busy;
    // Providers are user-wired entries; one failing provider never blocks the
    // others (its snapshot just carries the error).
    const entries = Array.isArray(this.cfg.providers) ? this.cfg.providers : [];
    this._busy = Promise.all(entries.map((p, i) => this._fetchProvider(p, p.type + ':' + i)))
      .then((results) => {
        const providers = {};
        entries.forEach((p, i) => { providers[p.type + ':' + i] = results[i]; });
        this.state = { enabled: true, providers, lastScan: new Date().toISOString() };
        this.emit('updated', this.state);
        return this.state;
      })
      .finally(() => { this._busy = null; });
    return this._busy;
  }

  async _fetchProvider(p, id) {
    if (!p || typeof p !== 'object') return emptyProvider('?', ['invalid provider entry']);
    let snap;
    if (p.enabled === false) {
      snap = emptyProvider(p.label || p.type);
      snap.status = 'disabled';
    } else if (p.type === 'chatgpt') {
      snap = await this._fetchChatgpt(p);
    } else if (p.type === 'zai') {
      snap = await this._fetchZai(p);
    } else {
      snap = emptyProvider(p.label || p.type, [`unknown provider type "${p.type}"`]);
    }
    // Reliability contract: a failed cycle never blanks the board. Keep the
    // last windows that actually came back from this provider and mark them
    // stale so the numbers are never mistaken for fresh ones. A DISABLED
    // provider shows its disabled state instead - stale windows are for
    // failed fetches, not for entries the user switched off.
    if (snap.ok && snap.windows && snap.windows.length) {
      this._lastGood.set(id, { windows: snap.windows, plan: snap.plan, fetchedAt: snap.fetchedAt });
    } else if (!snap.ok && this._lastGood.has(id)) {
      if (p.enabled === false) {
        this._lastGood.delete(id);
      } else {
        const good = this._lastGood.get(id);
        snap.windows = good.windows;
        snap.plan = snap.plan || good.plan;
        // Windows stay on the board but are marked stale so they are never
        // mistaken for fresh numbers (the pill shows 'stale').
        snap.status = 'stale';
      }
    }
    return snap;
  }

  // --- ChatGPT (via Codex CLI login) -----------------------------------------
  _readCodexAuth(authPath) {
    try {
      if (!authPath || !fs.existsSync(authPath)) return null;
      const auth = JSON.parse(fs.readFileSync(authPath, 'utf8'));
      if (auth.auth_mode && auth.auth_mode !== 'chatgpt') return null;
      const token = auth.tokens && auth.tokens.access_token;
      if (!token) return null;
      return { token };
    } catch (_) {
      return null;
    }
  }

  async _fetchChatgpt(p) {
    const label = p.label || 'ChatGPT';
    const auth = this._readCodexAuth(p.authPath);
    if (!auth) {
      const noAuth = emptyProvider(label, [`no ChatGPT login at ${p.authPath || '(authPath unset)'}`]);
      noAuth.status = 'no-auth';
      return noAuth;
    }
    try {
      const r = await this._fetch('https://chatgpt.com/backend-api/wham/usage', {
        headers: { Authorization: `Bearer ${auth.token}`, Accept: 'application/json' },
        signal: AbortSignal.timeout(this._timeoutFor(p))
      });
      if (!r.ok) {
        const err = emptyProvider(label, [`wham/usage HTTP ${r.status}`]);
        err.status = r.status === 401 || r.status === 403 ? 'auth-expired' : 'error';
        if (err.status === 'auth-expired') err.notes.push('open Codex once to refresh login');
        return err;
      }
      const body = await r.json();
      const rl = body.rate_limit || {};
      const windows = [];
      const pushWin = (key, wl, w) => {
        if (!w || typeof w.used_percent !== 'number') return;
        const usedPct = Math.min(100, Math.max(0, w.used_percent));
        windows.push({
          key,
          label: wl,
          percent: usedPct,
          remainingPercent: remainingPercent(usedPct),
          used: null,
          total: null,
          remaining: null,
          resetAt: w.reset_at ? Number(w.reset_at) * 1000 : null
        });
      };
      pushWin('primary', '5h', rl.primary_window);
      pushWin('secondary', 'week', rl.secondary_window);
      const status = rl.limit_reached ? 'blocked' : statusFromWindows(windows);
      return {
        ok: true,
        label,
        plan: body.plan_type || 'chatgpt',
        status,
        windows,
        notes: [],
        errors: [],
        fetchedAt: Date.now()
      };
    } catch (e) {
      const err = emptyProvider(label, [e.message || String(e)]);
      err.status = 'error';
      return err;
    }
  }

  // --- Z.ai coding plan (zcode credential) ------------------------------------
  _readZaiKey(p) {
    try {
      if (!p.configPath || !fs.existsSync(p.configPath)) return null;
      const cfg = JSON.parse(fs.readFileSync(p.configPath, 'utf8'));
      const entry = cfg.provider && cfg.provider[(p.provider || 'builtin:zai-coding-plan')];
      const key = entry && entry.options && entry.options.apiKey;
      if (typeof key === 'string' && key.length > 0 && entry.enabled !== false) return key;
      return null;
    } catch (_) {
      return null;
    }
  }

  /**
   * Identity headers the ZCode app sends. After app updates the quota
   * endpoint answers HTTP 200 with body {code:500} unless these are present.
   */
  _zaiIdentityHeaders(p, apiKey) {
    let mid = '';
    try {
      const tele = JSON.parse(fs.readFileSync(
        path.join(path.dirname(p.configPath || ''), 'telemetry-state.json'), 'utf8'));
      mid = tele.deviceMid || '';
    } catch (_) {}
    const VER = process.env.ZAI_ZCODE_VERSION || '3.11.2';
    let tz = 'UTC';
    try { tz = Intl.DateTimeFormat().resolvedOptions().timeZone || 'UTC'; } catch (_) {}
    return {
      authorization: apiKey,
      Accept: 'application/json',
      'x-api-key': apiKey,
      'User-Agent': `ZCode/${VER}`,
      'X-ZCode-App-Version': VER,
      'X-ZCode-Agent': 'glm',
      'X-Title': 'Z Code@electron',
      'HTTP-Referer': 'https://zcode.z.ai',
      'X-Release-Channel': 'production',
      'X-Client-Language': 'en-US',
      'X-Client-Timezone': tz,
      'X-Device-Mid': mid,
      'X-Platform': `${process.platform}-${process.arch}`,
      'X-Os-Category': process.platform,
      'X-Os-Version': os.release()
    };
  }

  async _fetchZai(p) {
    const label = p.label || 'Z.ai';
    const key = this._readZaiKey(p);
    if (!key) {
      const noAuth = emptyProvider(label, [`no coding-plan credential at ${p.configPath || '(configPath unset)'}`]);
      noAuth.status = 'no-auth';
      return noAuth;
    }
    try {
      let body = null;
      let lastMsg = '';
      // Bounded retry: one retry for the flaky body-code-500 shape, but the
      // WHOLE provider (all attempts) stays inside its configured deadline.
      const budget = this._timeoutFor(p);
      const t0 = Date.now();
      for (let attempt = 0; attempt < 2; attempt++) {
        const left = budget - (Date.now() - t0);
        if (attempt > 0 && left < 500) break; // no time for a meaningful retry
        if (attempt) await new Promise((r) => setTimeout(r, Math.min(500, left)));
        try {
          const r = await this._fetch('https://api.z.ai/api/monitor/usage/quota/limit', {
            headers: this._zaiIdentityHeaders(p, key),
            // Never exceed the budget: the floor is itself capped by what's left.
            signal: AbortSignal.timeout(Math.min(budget, Math.max(250, budget - (Date.now() - t0))))
          });
          const parsed = await r.json().catch(() => null);
          if (r.ok && parsed && parsed.success !== false && (!parsed.code || parsed.code === 200)) {
            body = parsed;
            break;
          }
          lastMsg = parsed && parsed.msg ? parsed.msg : `HTTP ${r.status}`;
          // body code 500 is the flaky/rejected shape — worth one retry
          if (parsed && parsed.code === 500) continue;
          break;
        } catch (e) {
          lastMsg = e.message || String(e);
          break; // network/timeout: do not hammer
        }
      }
      if (!body) {
        const err = emptyProvider(label, [`quota/limit ${lastMsg || 'no data'}`]);
        err.status = 'error';
        return err;
      }
      const limits = (body.data && body.data.limits) || [];
      const windows = limits.map((lim) => {
        const used = lim.currentValue ?? 0;
        const total = lim.usage ?? 0;
        const usedPct = percentLabel(used, total);
        return {
          key: lim.unit === 3 ? 'primary' : lim.unit === 6 ? 'secondary' : `u${lim.unit}`,
          label: windowLabel(lim.unit, lim.number),
          percent: usedPct,
          remainingPercent: remainingPercent(usedPct),
          used,
          total,
          remaining: lim.remaining ?? null,
          resetAt: typeof lim.nextResetTime === 'number' ? lim.nextResetTime : null
        };
      });
      const status = statusFromWindows(windows);
      return {
        ok: true,
        label,
        plan: (body.data && body.data.level) || 'coding',
        status,
        windows,
        notes: [],
        errors: [],
        fetchedAt: Date.now()
      };
    } catch (e) {
      const err = emptyProvider(label, [e.message || String(e)]);
      err.status = 'error';
      return err;
    }
  }
}

module.exports = { SubsTracker };
