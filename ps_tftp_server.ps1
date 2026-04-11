# ps_tftp_server.ps1 - TFTP Server
# Same approach as tftp_server_robust.ps1 (send DATA from port 69 socket),
# plus stale-drain so u-boot.itb starts without delay after idbloader.img.

param(
    [string]$TftpDir = 'C:\tftpboot',
    [string]$LocalIp  = ''          # if empty, auto-detect 192.168.1.x address
)

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]'Administrator')
if (-not $isAdmin) {
    Write-Host 'Warning: not running as Administrator; route pinning may be skipped.' -ForegroundColor Yellow
}

# Auto-detect the local IP on the 192.168.1.x subnet (Ethernet to the board).
if (-not $LocalIp) {
    $LocalIp = (Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object { $_.IPAddress -match '^192\.168\.' } |
        Select-Object -First 1).IPAddress
}
if (-not $LocalIp) { $LocalIp = '0.0.0.0' }

if (-not (Test-Path $TftpDir)) { New-Item -ItemType Directory -Path $TftpDir -Force | Out-Null }

# Ensure packets to 192.168.1.0/24 go through the Ethernet adapter, not VPN/tunnel.
if ($LocalIp -ne '0.0.0.0' -and $isAdmin) {
    try {
        $iface = (Get-NetIPAddress -IPAddress $LocalIp -ErrorAction Stop).InterfaceIndex
        # Remove any existing route that might point to a VPN, then add ours.
        Remove-NetRoute -DestinationPrefix '192.168.1.0/24' -InterfaceIndex $iface -Confirm:$false -ErrorAction SilentlyContinue
        New-NetRoute -DestinationPrefix '192.168.1.0/24' -InterfaceIndex $iface -RouteMetric 1 -ErrorAction SilentlyContinue | Out-Null
        Write-Host "Route 192.168.1.0/24 locked to interface $iface ($LocalIp)" -ForegroundColor DarkGreen
    } catch {
        Write-Host "Note: could not set route (non-fatal): $_" -ForegroundColor DarkYellow
    }
} elseif ($LocalIp -ne '0.0.0.0') {
    Write-Host 'Route pinning skipped because this shell is not elevated.' -ForegroundColor DarkYellow
}

Write-Host '=== PowerShell TFTP Server ===' -ForegroundColor Green
Write-Host "TFTP Directory : $TftpDir" -ForegroundColor Cyan
Write-Host "Local IP       : $LocalIp  (all DATA sent from this IP:69)" -ForegroundColor Cyan
Write-Host 'Press Ctrl+C to stop'
Write-Host ''
Write-Host 'TFTP Server is ready!' -ForegroundColor Green
Write-Host ''

# Send a file via TFTP to the given client endpoint.
# Uses the SAME $listener socket (port 69) for DATA -- same as tftp_server_robust.ps1.
# $listener is also passed for stale-drain after the transfer.
function Send-TftpFile([System.Net.Sockets.UdpClient]$listener,
                       [System.Net.IPEndPoint]$clientEP,
                       [string]$path) {

    $content = [System.IO.File]::ReadAllBytes($path)
    Write-Host "File size: $($content.Length) bytes" -ForegroundColor Cyan

    # No new TID socket - send from port 69 just like tftp_server_robust.ps1.
    # This avoids routing through neko-tun / VPN adapters.
    $tid = $listener
    $listener.Client.ReceiveTimeout = 2000
    $tidPort = 69
    Write-Host "Server TID port: $tidPort (same-socket mode)" -ForegroundColor DarkGray

    $bs = 512; $bn = 1; $off = 0; $ok = $true
    $lastBlock = ($content.Length % $bs) -eq 0   # need empty final block when exact multiple

    try {
        while ($off -lt $content.Length -or $lastBlock) {
            $chunk = [Math]::Min($bs, $content.Length - $off)
            # Build DATA packet
            $pkt = [byte[]]::new(4 + $chunk)
            $pkt[0] = 0; $pkt[1] = 3
            $pkt[2] = ($bn -shr 8) -band 0xFF; $pkt[3] = $bn -band 0xFF
            if ($chunk -gt 0) { [System.Array]::Copy($content, $off, $pkt, 4, $chunk) }

            $tid.Send($pkt, $pkt.Length, $clientEP) | Out-Null
            Write-Host "Sent blk $bn ($chunk bytes)" -ForegroundColor Gray

            $retry = 0; $maxR = 4; $got = $false
            $ep2 = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)

            while ($retry -lt $maxR -and -not $got) {
                try {
                    $a = $tid.Receive([ref]$ep2)
                    if ($a.Length -lt 4) { continue }
                    $aOp  = [int]$a[0] * 256 + [int]$a[1]
                    $aBlk = [int]$a[2] * 256 + [int]$a[3]
                    # Handle old U-Boot's low-byte-only ACK (e.g. ACK 0 for block 256)
                    $ackOk = ($aOp -eq 4) -and (
                        ($aBlk -eq $bn) -or
                        ((($bn -band 0xFF) -eq $aBlk) -and ($bn -ne $aBlk))
                    )
                    if ($ackOk) {
                        if ($aBlk -ne $bn) { Write-Host "ACK(lo) blk $bn" -ForegroundColor DarkYellow }
                        else               { Write-Host "ACK blk $bn"      -ForegroundColor Gray }
                        $got = $true; $off += $chunk; $bn++
                        if ($chunk -lt $bs) { $lastBlock = $false; break }
                    } elseif ($aOp -eq 4 -and $aBlk -lt $bn) {
                        Write-Host "Stale ACK $aBlk (want $bn)" -ForegroundColor Yellow
                    } elseif ($aOp -eq 1) {
                        Write-Host "Client re-RRQ: resend blk $bn" -ForegroundColor Yellow
                        $tid.Send($pkt, $pkt.Length, $clientEP) | Out-Null
                    }
                } catch {
                    $retry++
                    if ($retry -lt $maxR) {
                        Write-Host "Timeout blk $bn, retry $retry/$maxR" -ForegroundColor Yellow
                        $tid.Send($pkt, $pkt.Length, $clientEP) | Out-Null
                    } else {
                        Write-Host "Max retries blk $bn" -ForegroundColor Red
                    }
                }
            }
            if (-not $got) {
                Write-Host "Transfer FAILED at block $bn" -ForegroundColor Red
                $ok = $false; break
            }
        }
    } finally {
        # Do NOT close $tid - it is $listener (same socket). Restore timeout only.
        $listener.Client.ReceiveTimeout = 5000
    }

    if ($ok) { Write-Host "Transfer complete: $($content.Length) bytes" -ForegroundColor Green }
    else      { Write-Host 'Transfer incomplete' -ForegroundColor Red }

    # Drain stale RRQs that piled up in the UDP buffer while we were sending.
    # Old U-Boot retransmits RRQ every ~1s; after a large file these queue up.
    $listener.Client.ReceiveTimeout = 50
    $drainEP = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
    $drained = 0
    while ($true) {
        try { $listener.Receive([ref]$drainEP) | Out-Null; $drained++ }
        catch { break }
    }
    $listener.Client.ReceiveTimeout = 5000
    if ($drained -gt 0) { Write-Host "Drained $drained stale UDP packet(s)" -ForegroundColor DarkGray }
}

# ---- Main listener loop ----
# Bind to specific local IP so outbound DATA takes the correct Ethernet interface,
# not a VPN/tunnel adapter (e.g. neko-tun).
$bindIP = if ($LocalIp -ne '0.0.0.0') { [System.Net.IPAddress]::Parse($LocalIp) }
          else                          { [System.Net.IPAddress]::Any }
$listener = New-Object System.Net.Sockets.UdpClient
$listener.Client.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::Socket,
                                  [System.Net.Sockets.SocketOptionName]::ReuseAddress, $true)
$listener.Client.Bind([System.Net.IPEndPoint]::new($bindIP, 69))
$listener.Client.ReceiveTimeout = 5000
Write-Host "Bound to ${bindIP}:69" -ForegroundColor DarkGray

try {
    while ($true) {
        $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
        try { $data = $listener.Receive([ref]$ep) }
        catch [System.Net.Sockets.SocketException] { continue }

        Write-Host "Request from $($ep.Address):$($ep.Port)" -ForegroundColor Cyan

        $op = [int]$data[0] * 256 + [int]$data[1]
        if ($op -eq 1) {   # RRQ
            $fb = @()
            for ($i = 2; $i -lt $data.Length; $i++) {
                if ($data[$i] -eq 0) { break }
                $fb += $data[$i]
            }
            $fn = [System.Text.Encoding]::ASCII.GetString($fb)
            Write-Host "RRQ: $fn" -ForegroundColor Yellow
            $fp = Join-Path $TftpDir $fn
            if (Test-Path $fp) {
                Write-Host "File: $fp" -ForegroundColor Green
                Send-TftpFile $listener $ep $fp
            } else {
                Write-Host "Not found: $fp" -ForegroundColor Red
                $b = [System.Text.Encoding]::ASCII.GetBytes('File not found')
                $e = [byte[]]::new(4 + $b.Length + 1)
                $e[0] = 0; $e[1] = 5; $e[2] = 0; $e[3] = 1
                [System.Array]::Copy($b, 0, $e, 4, $b.Length)
                $listener.Send($e, $e.Length, $ep) | Out-Null
            }
        } else {
            Write-Host "Unexpected opcode $op from $($ep.Address)" -ForegroundColor Yellow
        }
    }
} finally {
    $listener.Close()
    Write-Host 'TFTP Server stopped' -ForegroundColor Yellow
}
