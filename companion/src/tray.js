// Tray icon through a hidden PowerShell helper (WinForms NotifyIcon), so the exe
// ships no extra binaries. Talks over the helper's stdin/stdout, one line per message:
//   node -> helper:  status|<#hex colour>|<tooltip>|<menu status line>
//                    startup|0 or 1
//                    front|<process name>,<process name>...   (just before launching an app)
//                    key|<virtual key code>                   (a media/volume key press)
//   helper -> node:  open | reconnect | startup | quit
//                    log|<text>
//                    media|<state>|<app>|<title>|<artist>     (what's playing, when it changes)
//
// "front" exists because Windows won't let a background process put another app's window
// in front: a launched app opens behind, or an already running one doesn't come up. The
// helper watches for that app's window and raises it (see Fg below).
// The helper exits when its stdin closes, so it never outlives the companion.

const { spawn } = require('node:child_process');
const { EventEmitter } = require('node:events');
const { log } = require('./log');

const SCRIPT = String.raw`
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

function Send($s) { [Console]::Out.WriteLine($s); [Console]::Out.Flush() }

Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class Fg {
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern int GetWindowTextLength(IntPtr h);
  [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern void mouse_event(uint f, int x, int y, uint d, UIntPtr e);
  [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e);

  // Visible, titled, unowned top-level windows of these processes, front-most first.
  public static long[] Windows(uint[] pids) {
    // PowerShell passes an empty array as null; that just means no windows yet.
    if (pids == null || pids.Length == 0) return new long[0];
    var want = new HashSet<uint>(pids);
    var found = new List<long>();
    EnumWindows((h, l) => {
      uint pid;
      GetWindowThreadProcessId(h, out pid);
      if (want.Contains(pid) && IsWindowVisible(h) && GetWindowTextLength(h) > 0 &&
          GetWindow(h, 4 /* GW_OWNER */) == IntPtr.Zero)
        found.Add(h.ToInt64());
      return true;
    }, IntPtr.Zero);
    return found.ToArray();
  }

  static bool Try(IntPtr h) { return SetForegroundWindow(h) && GetForegroundWindow() == h; }

  // Windows hands the foreground only to the process that made the last input, so if
  // asking plainly fails, make an input: a zero-distance mouse move first (invisible),
  // a lone Alt tap last (it can show menu or ribbon key hints in the app that had focus).
  public static string Raise(long handle) {
    var h = new IntPtr(handle);
    if (IsIconic(h)) ShowWindow(h, 9 /* SW_RESTORE */);
    if (Try(h)) return "ok";
    mouse_event(1 /* MOUSEEVENTF_MOVE */, 0, 0, 0, UIntPtr.Zero);
    if (Try(h)) return "ok after input";
    keybd_event(0x12, 0, 0, UIntPtr.Zero);
    keybd_event(0x12, 0, 2 /* KEYUP */, UIntPtr.Zero);
    return Try(h) ? "ok after alt" : "refused";
  }

  // A media or volume key, as if pressed on a keyboard (they are extended keys).
  public static void Key(byte vk) {
    keybd_event(vk, 0, 1 /* EXTENDEDKEY */, UIntPtr.Zero);
    keybd_event(vk, 0, 3 /* EXTENDEDKEY | KEYUP */, UIntPtr.Zero);
  }
}
'@

# ---- now playing: Windows' media session (what the volume flyout shows) ----
# WinRT calls are async; AsTask turns them into .NET tasks that the timer checks without
# blocking. If any of this is missing (old Windows), media info is just never sent.
$script:media = $null
try {
  Add-Type -AssemblyName System.Runtime.WindowsRuntime
  $script:asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -match '^IAsyncOperation.1$'   # IAsyncOperation, generic arity 1 (no backtick: this script is a JS template)
  } | Select-Object -First 1
  $script:mgrType = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType = WindowsRuntime]
  $script:propsType = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties, Windows.Media.Control, ContentType = WindowsRuntime]
  $script:media = @{ mgr = $null; mgrTask = $null; session = $null; propsTask = $null; next = [DateTime]::UtcNow; last = '' }
} catch { Send ("log|media info unavailable: " + $_.Exception.Message) }

function AsTask($op, [Type]$t) { $script:asTask.MakeGenericMethod($t).Invoke($null, @($op)) }
function Clean($s) { ([string]$s -replace '[|\r\n]', ' ').Trim() }

function ReportMedia($session, $props) {
  $state = 'none'; $app = ''; $title = ''; $artist = ''
  if ($session) {
    $st = [string]$session.GetPlaybackInfo().PlaybackStatus
    $state = @{ Playing = 'playing'; Paused = 'paused'; Stopped = 'stopped'; Changing = 'playing' }[$st]
    if (-not $state) { $state = 'none' }   # Closed / Opened: nothing to control
    $app = $session.SourceAppUserModelId
    if ($props) { $title = $props.Title; $artist = $props.Artist }
  }
  $line = 'media|' + $state + '|' + (Clean $app) + '|' + (Clean $title) + '|' + (Clean $artist)
  if ($line -ne $script:media.last) { $script:media.last = $line; Send $line }
}

function TickMedia {
  $m = $script:media
  if (-not $m.mgr) {
    if (-not $m.mgrTask) { $m.mgrTask = AsTask ($script:mgrType::RequestAsync()) $script:mgrType; return }
    if (-not $m.mgrTask.IsCompleted) { return }
    $m.mgr = $m.mgrTask.Result
    $m.mgrTask = $null
  }
  if ($m.propsTask) {
    if (-not $m.propsTask.IsCompleted) { return }
    $props = $null
    if (-not $m.propsTask.IsFaulted) { $props = $m.propsTask.Result }
    $m.propsTask = $null
    ReportMedia $m.session $props
    return
  }
  if ([DateTime]::UtcNow -lt $m.next) { return }
  $m.next = [DateTime]::UtcNow.AddSeconds(2)
  $m.session = $m.mgr.GetCurrentSession()
  if ($m.session) { $m.propsTask = AsTask ($m.session.TryGetMediaPropertiesAsync()) $script:propsType }
  else { ReportMedia $null $null }
}

# The leading comma keeps an empty result an empty array instead of PowerShell's $null.
function ProcIds($names) {
  ,[uint32[]]@(Get-Process -Name $names -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
}

# Apps waiting to be raised: their process names, the windows they had at launch, when.
$script:fronts = New-Object System.Collections.ArrayList

function TickFronts {
  foreach ($f in @($script:fronts)) {
    $wins = [Fg]::Windows((ProcIds $f.names))
    $age = ([DateTime]::UtcNow - $f.at).TotalMilliseconds
    $new = @($wins | Where-Object { $_ -notin $f.before })
    $pick = $null
    if ($new.Count) { $pick = $new[0] }                                  # it opened a window
    elseif ($f.before.Count -and $wins.Count -and $age -gt 1500) { $pick = $wins[0] }  # was running
    if ($pick) {
      Send ("log|front " + ($f.names -join ',') + ": " + [Fg]::Raise($pick))
      $script:fronts.Remove($f)
    } elseif ($age -gt 20000) {
      Send ("log|front " + ($f.names -join ',') + ": no window appeared")
      $script:fronts.Remove($f)
    }
  }
}

function MakeIcon($hex) {
  $bmp = New-Object System.Drawing.Bitmap 32, 32
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.Clear([System.Drawing.Color]::Transparent)
  $bg = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 30, 30, 28))
  $g.FillEllipse($bg, 0, 0, 31, 31)
  $track = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 70, 70, 66)), 5
  $g.DrawArc($track, 6, 6, 19, 19, 135, 270)
  $pen = New-Object System.Drawing.Pen ([System.Drawing.ColorTranslator]::FromHtml($hex)), 5
  $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
  $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
  $g.DrawArc($pen, 6, 6, 19, 19, 135, 190)
  $g.Dispose()
  return [System.Drawing.Icon]::FromHandle($bmp.GetHicon())
}

$ni = New-Object System.Windows.Forms.NotifyIcon
$ni.Icon = MakeIcon '#888780'
$ni.Text = 'claudeBoard'

$menu = New-Object System.Windows.Forms.ContextMenuStrip
$statusItem = $menu.Items.Add('starting...')
$statusItem.Enabled = $false
[void]$menu.Items.Add('-')
$open = $menu.Items.Add('Open settings')
$open.Font = New-Object System.Drawing.Font($open.Font, [System.Drawing.FontStyle]::Bold)
$open.add_Click({ Send 'open' })
$menu.Items.Add('Reconnect').add_Click({ Send 'reconnect' })
$startup = New-Object System.Windows.Forms.ToolStripMenuItem 'Start with Windows'
$startup.add_Click({ Send 'startup' })
[void]$menu.Items.Add($startup)
[void]$menu.Items.Add('-')
$menu.Items.Add('Quit').add_Click({ Send 'quit' })
$ni.ContextMenuStrip = $menu
$ni.add_MouseClick({ param($s, $e) if ($e.Button -eq [System.Windows.Forms.MouseButtons]::Left) { Send 'open' } })
$ni.Visible = $true

# Console.In blocks on ReadLineAsync in .NET Framework; a StreamReader of its own doesn't.
$reader = New-Object System.IO.StreamReader ([Console]::OpenStandardInput())
$script:task = $reader.ReadLineAsync()
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 150
$timer.add_Tick({
  if ($script:fronts.Count) { try { TickFronts } catch { Send ("log|front failed: " + $_.Exception.Message); $script:fronts.Clear() } }
  # A media error turns media info off rather than repeating every tick.
  if ($script:media) { try { TickMedia } catch { Send ("log|media info stopped: " + $_.Exception.Message); $script:media = $null } }
  while ($script:task.IsCompleted) {
    $line = $null
    if (-not $script:task.IsFaulted) { $line = $script:task.Result }
    if ($line -eq $null) { $timer.Stop(); $ni.Visible = $false; [System.Windows.Forms.Application]::Exit(); return }
    $p = $line.Split('|')
    if ($p[0] -eq 'status' -and $p.Length -ge 4) {
      $old = $ni.Icon
      $ni.Icon = MakeIcon $p[1]
      if ($old) { $old.Dispose() }
      $t = $p[2]; if ($t.Length -gt 63) { $t = $t.Substring(0, 63) }
      $ni.Text = $t
      $statusItem.Text = $p[3]
    } elseif ($p[0] -eq 'startup') {
      $startup.Checked = ($p[1] -eq '1')
    } elseif ($p[0] -eq 'key' -and $p.Length -ge 2) {
      try {
        [Fg]::Key([byte][int]$p[1])
        if ($script:media) { $script:media.next = [DateTime]::UtcNow.AddMilliseconds(400) }   # show the effect soon
      } catch { Send ("log|key failed: " + $_.Exception.Message) }
    } elseif ($p[0] -eq 'front' -and $p.Length -ge 2) {
      # Never let this throw: the tick would end before the next stdin read is armed.
      try {
        $names = @($p[1].Split(',') | Where-Object { $_ })
        if ($names.Count) {
          [void]$script:fronts.Add(@{ names = $names; at = [DateTime]::UtcNow; before = [Fg]::Windows((ProcIds $names)) })
        }
      } catch { Send ("log|front failed: " + $_.Exception.Message) }
    }
    $script:task = $reader.ReadLineAsync()
  }
})
$timer.Start()
[System.Windows.Forms.Application]::Run()
$ni.Dispose()
`;

class Tray extends EventEmitter {
  constructor() {
    super();
    this.child = null;
    this.quitting = false;
    this.restarts = 0;
    this.last = { status: null, startup: null };
  }

  start() {
    if (process.platform !== 'win32') return;
    const encoded = Buffer.from(SCRIPT, 'utf16le').toString('base64');
    const child = spawn('powershell.exe',
      ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-STA', '-EncodedCommand', encoded],
      { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
    this.child = child;
    let buf = '';
    child.stdout.on('data', (d) => {
      buf += d.toString();
      let i;
      while ((i = buf.indexOf('\n')) >= 0) {
        const cmd = buf.slice(0, i).trim();
        buf = buf.slice(i + 1);
        if (cmd.startsWith('log|')) log('tray:', cmd.slice(4));
        else if (cmd.startsWith('media|')) {
          const [, state, app, title, artist] = cmd.split('|');
          this.emit('media', { state, app: app || '', title: title || '', artist: artist || '' });
        }
        else if (cmd) this.emit('command', cmd);
      }
    });
    let err = '';
    child.stderr.on('data', (d) => { err = (err + d.toString()).slice(-2000); });
    child.stdin.on('error', () => {});
    child.on('error', (e) => log('tray:', e.message));
    child.on('exit', (code) => {
      if (this.child === child) this.child = null;
      if (this.quitting) return;
      log(`tray helper exited (${code})${err ? ': ' + err.trim().split('\n').slice(-3).join(' ') : ''}`);
      if (this.restarts++ < 3) setTimeout(() => this.start(), 3000);
    });
    // Replay the last state into a fresh helper.
    if (this.last.status) this.write(this.last.status);
    if (this.last.startup) this.write(this.last.startup);
  }

  write(line) {
    if (this.child && this.child.stdin.writable) this.child.stdin.write(line + '\n');
  }

  setStatus(color, tooltip, line) {
    const clean = (s) => String(s).replace(/[|\r\n]/g, ' ');
    const msg = `status|${color}|${clean(tooltip)}|${clean(line)}`;
    if (msg === this.last.status) return;
    this.last.status = msg;
    this.write(msg);
  }

  setStartup(on) {
    this.last.startup = `startup|${on ? 1 : 0}`;
    this.write(this.last.startup);
  }

  // Press a key: only the media and volume keys are let through.
  key(vk) {
    if ([0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb3].includes(vk)) this.write(`key|${vk}`);
  }

  // Raise the next window of any of these processes (names without .exe).
  front(names) {
    const clean = names.map((n) => String(n).replace(/[^\w.-]/g, '')).filter(Boolean);
    if (clean.length) this.write(`front|${[...new Set(clean)].join(',')}`);
  }

  stop() {
    this.quitting = true;
    if (this.child) this.child.stdin.end();
  }
}

module.exports = { Tray };
