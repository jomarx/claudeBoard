// Text for the board: its fonts are ASCII only. Typographic punctuation (common in song
// titles) becomes its plain form, accents are dropped ("São Paulo" -> "Sao Paulo",
// "Beyoncé" -> "Beyonce"), and anything still outside ASCII becomes '?'.
function ascii(s, max) {
  return String(s)
    .replace(/[‐-―−]/g, '-')        // hyphens and dashes, minus
    .replace(/[‘’‚′]/g, "'")   // single quotes, prime
    .replace(/[“”„″]/g, '"')   // double quotes
    .replace(/…/g, '...')
    .replace(/[  -​ ]/g, ' ')  // odd spaces
    .normalize('NFD').replace(/[̀-ͯ]/g, '')
    .replace(/[^\x20-\x7e]/g, '?')
    .slice(0, max);
}

module.exports = { ascii };
