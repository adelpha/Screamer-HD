<#
.SYNOPSIS
  Reads emulated memory from a running DOSBox Staging started with
  run-original.ps1 -Webserver (REST API on 127.0.0.1:8086).

.EXAMPLE
  .\tools\dosbox\read-memory.ps1 -Address 0xA0000 -Length 64000 -OutFile artifacts\vga.bin   # mode 13h screen
  .\tools\dosbox\read-memory.ps1 -Address 0x2B5000 -Length 256 -Hex                         # hexdump
  .\tools\dosbox\read-memory.ps1 -Cpu                                                       # registers as JSON

  Other endpoints (see http://127.0.0.1:8086/ while DOSBox runs):
    GET  /api/v1/memory/:segment/:offset/:len   segment may be a register name (ds, es, ...)
    PUT  /api/v1/memory/:offset                 write memory (octet-stream body)
    GET  /api/v1/dos/internals
#>
param(
    [string] $Address = '0',
    [int]    $Length = 256,
    [string] $Segment,
    [string] $OutFile,
    [switch] $Hex,
    [switch] $Cpu,
    [int]    $Port = 8086
)
$ErrorActionPreference = 'Stop'
$base = "http://127.0.0.1:$Port/api/v1"
if ($Cpu) { Invoke-RestMethod "$base/cpu/state" | ConvertTo-Json -Depth 4; return }

$url = if ($Segment) { "$base/memory/$Segment/$Address/$Length" } else { "$base/memory/$Address/$Length" }
$tmp = if ($OutFile) { $OutFile } else { [IO.Path]::GetTempFileName() }
Invoke-WebRequest $url -OutFile $tmp -UseBasicParsing
if ($Hex -or -not $OutFile) {
    $bytes = [IO.File]::ReadAllBytes((Resolve-Path $tmp))
    $start = [Convert]::ToInt64($Address, $(if ($Address -like '0x*') { 16 } else { 10 }))
    for ($i = 0; $i -lt $bytes.Length; $i += 16) {
        $row = $bytes[$i..([Math]::Min($i + 15, $bytes.Length - 1))]
        $hexs = ($row | ForEach-Object { $_.ToString('x2') }) -join ' '
        $ascii = -join ($row | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { '.' } })
        '{0:x8}  {1,-47}  {2}' -f ($start + $i), $hexs, $ascii
    }
}
if (-not $OutFile) { Remove-Item $tmp }
