// Current weather for the place set on the settings page, from Open-Meteo (free, no API
// key). Only the saved latitude/longitude go out, to api.open-meteo.com; place searches go
// to geocoding-api.open-meteo.com. The board gets a compact "weather" frame (src/protocol.h).

const { log } = require('./log');

const REFRESH_MS = 15 * 60 * 1000;
const RETRY_MS = 2 * 60 * 1000;     // after a failed fetch
const TIMEOUT_MS = 10000;

// seq goes up every time new data (or a cleared location) should reach the board.
const weatherState = { data: null, error: '', at: 0, seq: 0, key: '' };

const round = (v) => (Number.isFinite(v) ? Math.round(v) : null);

// The board's fonts are ASCII only: drop accents ("São Paulo" -> "Sao Paulo"), then anything left.
function ascii(s, max) {
  return String(s).normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/[^\x20-\x7e]/g, '?').slice(0, max);
}

async function getJSON(url) {
  const r = await fetch(url, { signal: AbortSignal.timeout(TIMEOUT_MS) });
  if (!r.ok) throw new Error(`${new URL(url).host} answered ${r.status}`);
  return r.json();
}

// Fetches when the location changed, when forced, or every REFRESH_MS. Never throws.
async function refreshWeather(cfg, force = false) {
  const w = cfg.weather;
  const key = w.lat === null ? '' : `${w.lat},${w.lon}`;
  if (!key) {
    if (weatherState.key) Object.assign(weatherState, { data: null, error: '', at: 0, key: '', seq: weatherState.seq + 1 });
    return;
  }
  if (!force && key === weatherState.key && Date.now() - weatherState.at < REFRESH_MS) return;
  if (key !== weatherState.key) {
    // A new place: the old reading must not be shown under the new name, even if this
    // fetch fails, so drop it now (and let the board know).
    weatherState.data = null;
    weatherState.seq++;
  }
  weatherState.at = Date.now();
  weatherState.key = key;
  const q = new URLSearchParams({
    latitude: String(w.lat), longitude: String(w.lon), timezone: 'auto', forecast_days: '1',
    current: 'temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,wind_speed_10m',
    daily: 'temperature_2m_max,temperature_2m_min,precipitation_probability_max',
  });
  try {
    const j = await getJSON(`https://api.open-meteo.com/v1/forecast?${q}`);
    const c = j.current || {}, d = j.daily || {};
    const first = (a) => (Array.isArray(a) ? a[0] : undefined);
    const t = round(c.temperature_2m);
    if (t === null) throw new Error('no current temperature in the answer');
    weatherState.data = {
      t,
      feels: round(c.apparent_temperature) ?? t,
      hum: round(c.relative_humidity_2m) ?? 0,
      wind: round(c.wind_speed_10m) ?? 0,
      code: round(c.weather_code) ?? 0,
      day: c.is_day === 0 ? 0 : 1,
      hi: round(first(d.temperature_2m_max)) ?? t,
      lo: round(first(d.temperature_2m_min)) ?? t,
      rain: round(first(d.precipitation_probability_max)) ?? -1,
      // Open-Meteo gives local time without an offset; it's "now" to the 15 minutes anyway.
      at: Math.floor(Date.now() / 1000),
    };
    weatherState.error = '';
    weatherState.seq++;
  } catch (e) {
    const msg = e.name === 'TimeoutError' ? 'weather request timed out' : e.message;
    if (msg !== weatherState.error) log('weather:', msg);
    weatherState.error = msg;
    // Try again in 2 minutes rather than waiting out the full 15.
    weatherState.at = Date.now() - REFRESH_MS + RETRY_MS;
  }
}

function weatherFrame(cfg) {
  if (!weatherState.data || cfg.weather.lat === null) return { type: 'weather', valid: false };
  return { type: 'weather', place: ascii(cfg.weather.name || 'Weather', 23), ...weatherState.data };
}

// Place search for the settings page: up to 6 matches with a readable label.
async function geocode(query) {
  const name = String(query || '').trim().slice(0, 80);
  if (name.length < 2) return [];
  const q = new URLSearchParams({ name, count: '6', language: 'en', format: 'json' });
  const j = await getJSON(`https://geocoding-api.open-meteo.com/v1/search?${q}`);
  return (j.results || []).map((r) => ({
    name: r.name,
    label: [r.name, r.admin1, r.country].filter(Boolean).join(', '),
    lat: r.latitude,
    lon: r.longitude,
  }));
}

module.exports = { refreshWeather, weatherFrame, weatherState, geocode, REFRESH_MS };
