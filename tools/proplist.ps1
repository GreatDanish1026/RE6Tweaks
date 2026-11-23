# Lists MT Framework property registrations in a code range of BH6.exe: name, type and the
# field offset (plain fields: lea reg,[ebx+off] stored at [esp+1Ch]) or getter/setter
# (type flag 0x800000) with the getter's first instruction (which shows the field it reads).
# Usage: tools\proplist.ps1 -From 0x1044630 -To 0x1045460
param([string]$From, [string]$To, [string]$Exe = "E:\SteamLibrary\steamapps\common\Resident Evil 6\BH6.exe")
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$db = (Get-ChildItem "$vs\VC\Tools\MSVC" -Recurse -Filter dumpbin.exe | Where-Object FullName -match 'Hostx64\\x86' | Select-Object -First 1).FullName
$bytes = [IO.File]::ReadAllBytes($Exe)
function Get-Str([int64]$va) {
    $off = $va - 0x400000 - 0x1102000 + 0x1100600
    if ($off -lt 0 -or $off -ge $bytes.Length) { return "?" }
    $end = [Array]::IndexOf($bytes, [byte]0, [int]$off)
    return [Text.Encoding]::ASCII.GetString($bytes, $off, $end - $off)
}
function Get-First([int64]$va) {
    $out = & $db /nologo /disasm:nobytes ("/range:0x{0:x},0x{1:x}" -f $va, ($va + 16)) $Exe
    ($out | Where-Object { $_ -match '^\s+[0-9A-F]{8}:' } | Select-Object -First 2 |
        ForEach-Object { ($_ -replace '^\s+\w+:\s+', '') -replace '\s+', ' ' }) -join ' ; '
}
$lines = & $db /nologo /disasm:nobytes "/range:$From,$To" $Exe
$cur = @{}
$lea = @{}
foreach ($l in $lines) {
    if ($l -match 'lea\s+(\w+),\[ebx\+([0-9A-F]+)h\]') { $lea[$Matches[1]] = [Convert]::ToInt64($Matches[2], 16) }
    elseif ($l -match 'lea\s+(\w+),\[ebx\]') { $lea[$Matches[1]] = 0 }
    if ($l -match 'mov\s+dword ptr \[esp\+(10|14|18|1C|24)h\],([0-9A-F]+)h?$') {
        $cur[$Matches[1]] = [Convert]::ToInt64($Matches[2], 16)
    }
    elseif ($l -match 'mov\s+dword ptr \[esp\+1Ch\],(e[a-d]x|e[sd]i|ebp)$' -and $lea.ContainsKey($Matches[1])) {
        $cur['off'] = $lea[$Matches[1]]
    }
    if ($l -match '\scall\s' -and $cur.ContainsKey('10')) {
        $name = Get-Str $cur['10']
        $type = $cur['14']
        if ($type -band 0x800000) {
            "{0,-34} type {1,6:x}  get {2:x8} set {3:x8} | {4}" -f $name, ($type -band 0xffff), $cur['1C'], $cur['24'], (Get-First $cur['1C'])
        } elseif ($cur.ContainsKey('off')) {
            "{0,-34} type {1,6:x}  field +0x{2:x}" -f $name, ($type -band 0xffff), $cur['off']
        } else {
            "{0,-34} type {1,6:x}  ?" -f $name, ($type -band 0xffff)
        }
        $cur = @{}
    }
}
