// The board's media screen: what's playing (reported by the tray helper from Windows' media
// session) goes down as a "media" frame, and its buttons come back as media keys.

const { ascii } = require('./text');

// MediaKey order in src/ui.h: previous, play/pause, next, volume down, mute, volume up.
const MEDIA_VK = [0xb1, 0xb3, 0xb0, 0xae, 0xad, 0xaf];

// Windows names the player by its app id: "Chrome", "Spotify.exe",
// "SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify", "Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic".
function appLabel(id) {
  let s = String(id || '');
  if (s.includes('!')) s = s.slice(s.lastIndexOf('!') + 1);
  s = s.replace(/\.exe$/i, '').split(/[\\/]/).pop();
  if (s.includes('.')) s = s.slice(s.lastIndexOf('.') + 1);   // Microsoft.ZuneMusic -> ZuneMusic
  return s ? s[0].toUpperCase() + s.slice(1) : '';
}

const STATES = ['playing', 'paused', 'stopped', 'none'];

function mediaFrame(m) {
  const state = STATES.includes(m && m.state) ? m.state : 'none';
  if (state === 'none') return { type: 'media', state };
  return {
    type: 'media', state,
    app: ascii(appLabel(m.app), 15),
    title: ascii(m.title || '', 47),
    artist: ascii(m.artist || '', 31),
  };
}

module.exports = { MEDIA_VK, mediaFrame, appLabel };
