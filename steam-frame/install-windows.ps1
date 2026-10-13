# install-windows.ps1 - installs, updates or removes CitraVR - Steam Frame Edition on a Steam
# Frame from a Windows 10/11 PC. Double-click install-windows.cmd, which runs it, or paste this
# in PowerShell:
#
#   irm https://raw.githubusercontent.com/SPD13/CitraVR-steam-frame/master/steam-frame/install-windows.ps1 | iex
#
# It asks what to do and the headset's address, checks that the headset answers, then logs in to
# it with Windows' ssh client (as steamos, with the Developer Mode password) and runs the latest
# release's install.sh there, which does the install itself (see install.sh). If the ssh client
# is missing, it offers to add it (Windows' optional OpenSSH Client feature).
#
# Written for Windows PowerShell 5.1, so no PowerShell 7 syntax, and ASCII only. Everything runs
# in a script block, so `irm | iex` leaves nothing behind in the PowerShell session, and it never
# calls exit, which would close that session's window.

& {
  $ErrorActionPreference = 'Stop'
  $repo = 'SPD13/CitraVR-steam-frame'
  $installer = "https://github.com/$repo/releases/latest/download/install.sh"
  $help = "https://github.com/$repo#troubleshooting"

  function Say([string]$text, [string]$color = 'Gray') { Write-Host $text -ForegroundColor $color }
  function Fail([string]$text) { Write-Host "error: $text" -ForegroundColor Red }

  function Find-Ssh {
    $cmd = Get-Command ssh -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) { return $cmd.Path }
    # Windows' OpenSSH when it isn't on PATH yet (just added), or seen from 32-bit PowerShell
    if ($env:windir) {
      foreach ($dir in 'System32', 'Sysnative') {
        $path = Join-Path $env:windir "$dir\OpenSSH\ssh.exe"
        if (Test-Path $path) { return $path }
      }
    }
    return $null
  }

  # 'ok', or why the headset's ssh port doesn't answer: 'notfound', 'refused' or 'timeout'
  function Test-Headset([string]$address) {
    $client = New-Object System.Net.Sockets.TcpClient
    try {
      if ($client.ConnectAsync($address, 22).Wait(8000) -and $client.Connected) { return 'ok' }
      return 'timeout'
    } catch {
      $e = $_.Exception
      while ($e.InnerException) { $e = $e.InnerException }
      if ($e -is [System.Net.Sockets.SocketException]) {
        switch ($e.SocketErrorCode.ToString()) {
          'HostNotFound' { return 'notfound' }
          'NoData' { return 'notfound' }
          'TryAgain' { return 'notfound' }
          'ConnectionRefused' { return 'refused' }
        }
      }
      return 'timeout'
    } finally {
      $client.Close()
    }
  }

  Say ''
  Say 'CitraVR - Steam Frame Edition installer' Cyan
  Say ''
  Say 'Before you start, the headset must be on and awake, on the same network as this PC, with'
  Say 'Developer Mode turned on and a password set (in the headset: Steam Settings > System).'

  $ssh = Find-Ssh
  if (-not $ssh) {
    Say ''
    Say "Windows' ssh client (OpenSSH Client) isn't installed. It's an optional feature of Windows 10/11." Yellow
    $answer = Read-Host 'Add it now? Windows asks for administrator rights [Y/n]'
    if ($answer.Trim() -notmatch '^(n|no)$') {
      Say 'Adding OpenSSH Client, this can take a minute...'
      try {
        Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile', '-Command',
          'Add-WindowsCapability -Online -Name OpenSSH.Client~~~~0.0.1.0'
      } catch {
        Fail 'Windows did not add it (administrator rights refused?).'
      }
      $ssh = Find-Ssh
    }
    if (-not $ssh) {
      Fail 'no ssh client. Add it in Settings > System > Optional features > Add a feature > OpenSSH Client, then run this installer again.'
      return
    }
  }

  Say ''
  Say '  1. Install or update CitraVR (latest release)'
  Say '  2. Uninstall CitraVR (your games and saves stay on the headset)'
  $choice = $null
  while (-not $choice) {
    $answer = (Read-Host 'Choose 1 or 2 [1]').Trim()
    if ($answer -eq '' -or $answer -eq '1') { $choice = 'install' }
    elseif ($answer -eq '2') { $choice = 'uninstall' }
  }

  Say ''
  Say "Enter the headset's address, or press Enter for frame.local. If frame.local isn't found, use"
  Say "the headset's IP address, shown in its Wi-Fi settings (for example 192.168.1.42)."
  $headset = $null
  while (-not $headset) {
    $answer = (Read-Host 'Headset address [frame.local], or q to quit').Trim()
    if ($answer -eq 'q') { return }
    if ($answer -eq '') { $answer = 'frame.local' }
    Say "Looking for the headset at $answer..."
    switch (Test-Headset $answer) {
      'ok' { $headset = $answer }
      'notfound' { Fail "$answer wasn't found on the network. Wake the headset, or enter its IP address." }
      'refused' { Fail "$answer doesn't accept ssh logins. Turn on Developer Mode in the headset (Steam Settings > System)." }
      default { Fail "no answer from $answer. Wake the headset and check it's on the same network as this PC." }
    }
  }

  $remote = "curl -fsSL $installer | bash"
  if ($choice -eq 'uninstall') { $remote += ' -s -- --uninstall' }

  Say ''
  Say "Connecting to $headset as steamos. Enter the Developer Mode password when asked (it" Cyan
  Say "doesn't show as you type)." Cyan
  # install.sh prints UTF-8 (ellipses, quotes)
  try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
  # accept-new trusts the headset's key the first time, without the yes/no question, and still
  # refuses a key that changed since
  & $ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=15 "steamos@$headset" $remote
  $code = $LASTEXITCODE

  Say ''
  if ($code -eq 0) {
    if ($choice -eq 'uninstall') {
      Say 'CitraVR was removed from the Steam library.' Green
    } else {
      Say 'Done. In the headset, open Library > Non-Steam > Citra VR - Steam Frame Edition and press Play.' Green
    }
  } elseif ($code -eq 255) {
    Fail "couldn't log in to the headset. Check the Developer Mode password, then run this installer again."
  } else {
    Fail "the installer on the headset stopped (see its message above). Troubleshooting: $help"
  }
}
