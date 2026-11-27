// Tray icon through a hidden PowerShell helper (WinForms NotifyIcon), so the exe
// ships no extra binaries. Talks over the helper's stdin/stdout, one line per message:
//   node -> helper:  status|<#hex colour>|<tooltip>|<menu status line>
//                    startup|0 or 1
//   helper -> node:  open | reconnect | startup | quit
// The helper exits when its stdin closes, so it never outlives the companion.

const { spawn } = require('node:child_process');
const { EventEmitter } = require('node:events');
const { log } = require('./log');

const SCRIPT = String.raw`
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

function Send($s) { [Console]::Out.WriteLine($s); [Console]::Out.Flush() }

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
        if (cmd) this.emit('command', cmd);
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

  stop() {
    this.quitting = true;
    if (this.child) this.child.stdin.end();
  }
}

module.exports = { Tray };
