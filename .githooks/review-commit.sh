#!/bin/sh
# Background half of the post-commit review. Usage: review-commit.sh <commit sha>
#
# Runs `claude -p` with read-only tools (file reads, search, git show/diff/log/blame) and
# writes reviews/<time>-<sha>.md. Every review also gets a line in reviews/INDEX.md, and a
# Windows notification pops up when a review finds something.

sha="$1"
root=$(git rev-parse --show-toplevel) || exit 0
cd "$root" || exit 0
short=$(git rev-parse --short "$sha")
subject=$(git log -1 --format=%s "$sha")
stamp=$(date +%Y-%m-%d\ %H:%M)
out="reviews/$(date +%Y%m%d-%H%M%S)-$short.md"
mkdir -p reviews

prompt="Review commit $sha of this repository for real defects. The repo is claudeBoard: ESP32
firmware in src/ (PlatformIO, TFT_eSPI) and a Node companion in companion/ that talk JSON over
serial. Read CLAUDE.md first: it lists rules a change must follow (for example, a new board
setting must be added in config.js normalize(), the page, Settings in src/model.h,
applyConfig/protocolSendSettings and settings.cpp; the protocol must change in src/protocol.h
and the companion together).

Look at the commit with git show $sha, and read the surrounding code it touches. Report only:
bugs and crashes, protocol or setting mismatches between the firmware and the companion, broken
CLAUDE.md rules, security problems, and drawing that overlaps or leaves the 240x320 / 320x240
screen. No style notes, no praise, nothing speculative you could not point to in the code.

Output format, exactly: the first line is 'FINDINGS: <number>'. Then, per finding, a '## ' heading
with file:line and a one-line summary, followed by what is wrong and a concrete case where it
fails. If there are none, the first line is 'FINDINGS: 0' and one more line says what you checked."

claude -p "$prompt" \
  --allowedTools "Read" "Grep" "Glob" "Bash(git show:*)" "Bash(git diff:*)" "Bash(git log:*)" "Bash(git blame:*)" \
  --output-format text > "$out.tmp" 2> reviews/last-error.log
status=$?

count=$(sed -n '1s/^FINDINGS: *\([0-9][0-9]*\).*/\1/p' "$out.tmp")
if [ $status -ne 0 ] || [ -z "$count" ]; then
  count="?"
  result="review failed (exit $status), see reviews/last-error.log"
else
  result="$count finding(s)"
fi

{
  echo "# Review of $short: $subject"
  echo
  echo "$stamp, $result"
  echo
  cat "$out.tmp"
} > "$out"
rm -f "$out.tmp"
[ -f reviews/INDEX.md ] || printf '# Commit reviews\n\n| When | Commit | Result | Subject |\n|---|---|---|---|\n' > reviews/INDEX.md
echo "| $stamp | $short | $result | $subject |" >> reviews/INDEX.md

# Notify only when there's something to look at.
if [ "$count" != "0" ]; then
  title="claudeBoard review: $short"
  body="$result - $(basename "$out")"
  powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -Command "
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    \$n = New-Object System.Windows.Forms.NotifyIcon
    \$n.Icon = [System.Drawing.SystemIcons]::Warning
    \$n.Visible = \$true
    \$n.ShowBalloonTip(15000, '$title', '$body', [System.Windows.Forms.ToolTipIcon]::Warning)
    Start-Sleep -Seconds 15
    \$n.Dispose()" >/dev/null 2>&1
fi
