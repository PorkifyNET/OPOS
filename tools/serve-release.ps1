# Serves a release folder over HTTP so OPOS in a VM can update from the host.
#
#   powershell -ExecutionPolicy Bypass -File tools\serve-release.ps1 [-Folder build\release] [-Port 8000]
#
# In OPOS: update install --source http://HOST-IP:8000/
# (or put that URL in /disk/OPOS/UPDATE-SOURCE.TXT for Settings > UPDATES).
# HOST-IP is the host's address on the VM's network, for VMware NAT the
# "VMware Network Adapter VMnet8" address shown by ipconfig. Windows may ask
# to allow PowerShell through the firewall: allow it on private networks.
# Updates are signature-checked, so serving over plain HTTP is safe. Stop
# with Ctrl+C.
param(
    [string]$Folder = (Join-Path $PSScriptRoot "..\build\release"),
    [int]$Port = 8000
)

$root = (Resolve-Path $Folder).Path
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Any, $Port)
$listener.Start()
Write-Host "Serving $root on port $Port (Ctrl+C stops)"
Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -ne "127.0.0.1" } |
    ForEach-Object { Write-Host ("  http://{0}:{1}/   ({2})" -f $_.IPAddress, $Port, $_.InterfaceAlias) }

try {
    while ($true) {
        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $reader = [System.IO.StreamReader]::new($stream, [System.Text.Encoding]::ASCII)
            $request = $reader.ReadLine()
            while (($line = $reader.ReadLine()) -ne $null -and $line -ne "") { }
            $name = ""
            if ($request -match '^GET /([^ ?]*)') { $name = [System.Uri]::UnescapeDataString($Matches[1]) }
            # Only plain file names in the folder itself.
            $path = Join-Path $root $name
            if ($name -match '^[A-Za-z0-9._-]+$' -and (Test-Path -LiteralPath $path -PathType Leaf)) {
                $bytes = [System.IO.File]::ReadAllBytes($path)
                $header = "HTTP/1.0 200 OK`r`nContent-Length: $($bytes.Length)`r`nContent-Type: application/octet-stream`r`nConnection: close`r`n`r`n"
                Write-Host "200 $name ($($bytes.Length) bytes)"
            } else {
                $bytes = [byte[]]::new(0)
                $header = "HTTP/1.0 404 Not Found`r`nContent-Length: 0`r`nConnection: close`r`n`r`n"
                Write-Host "404 $name"
            }
            $head = [System.Text.Encoding]::ASCII.GetBytes($header)
            $stream.Write($head, 0, $head.Length)
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush()
        } catch {
            Write-Host "error: $_"
        } finally {
            $client.Close()
        }
    }
} finally {
    $listener.Stop()
}
