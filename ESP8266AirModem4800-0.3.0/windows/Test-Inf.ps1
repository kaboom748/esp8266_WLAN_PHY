# Read-only validation. No device installation, registry writes or COM opens.
[CmdletBinding()]
param([string]$InfPath = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $InfPath) {
    $InfPath = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'airmodem-transparent.inf'
}
$InfPath = (Resolve-Path -LiteralPath $InfPath).Path

if (-not ('AirModemInfReader' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
public sealed class AirModemInfReader : IDisposable {
    [StructLayout(LayoutKind.Sequential)]
    struct Context { public IntPtr Inf, CurrentInf; public uint Section, Line; }
    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr SetupOpenInfFileW(string path, string cls, uint style, out uint line);
    [DllImport("setupapi.dll")] static extern void SetupCloseInfFile(IntPtr inf);
    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool SetupFindFirstLineW(IntPtr inf, string section, string key, out Context ctx);
    [DllImport("setupapi.dll", SetLastError=true)]
    static extern bool SetupFindNextLine(ref Context ctx, out Context next);
    [DllImport("setupapi.dll")] static extern uint SetupGetFieldCount(ref Context ctx);
    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool SetupGetStringFieldW(ref Context ctx, uint field,
        StringBuilder buffer, uint capacity, out uint required);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool BuildCommDCBW(string spec, [In, Out] byte[] dcb);
    IntPtr handle;
    public AirModemInfReader(string path) {
        uint line;
        handle=SetupOpenInfFileW(path, "Modem", 2, out line);
        if (handle==new IntPtr(-1))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "INF line " + line);
    }
    public List<string[]> Read(string section) {
        Context ctx;
        if (!SetupFindFirstLineW(handle, section, null, out ctx))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Section " + section);
        var rows=new List<string[]>();
        do {
            uint count=SetupGetFieldCount(ref ctx);
            var row=new string[count+1];
            for (uint n=0; n<=count; n++) {
                uint required;
                var buffer=new StringBuilder(8192);
                if (!SetupGetStringFieldW(ref ctx,n,buffer,8192,out required)) {
                    // AddReg lines have no key to the left of '=' (field zero).
                    if (n==0) { row[n]=""; continue; }
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                }
                row[n]=buffer.ToString();
            }
            rows.Add(row);
            Context next;
            if (!SetupFindNextLine(ref ctx,out next)) {
                int error=Marshal.GetLastWin32Error();
                if (error!=unchecked((int)0xe0000102)) throw new Win32Exception(error);
                break;
            }
            ctx=next;
        } while (true);
        return rows;
    }
    public static byte[] BuildDcb() {
        var dcb=new byte[28];
        dcb[0]=28; dcb[8]=0x81; // fBinary + independent TX after locally sent XOFF.
        if (!BuildCommDCBW("baud=4800 parity=N data=8 stop=1 xon=on odsr=off octs=off dtr=off rts=off idsr=off",dcb))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        return dcb;
    }
    public void Dispose() {
        if (handle!=IntPtr.Zero && handle!=new IntPtr(-1)) SetupCloseInfFile(handle);
        handle=IntPtr.Zero;
    }
}
'@
}

$script:checks = 0
function Assert-Inf([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAIL: $Message" }
    $script:checks++
}
function Get-RegRow([string]$Section, [string]$Key, [string]$Name) {
    $matches = @($reader.Read($Section) | Where-Object {
        $_[1] -eq 'HKR' -and $_[2] -eq $Key -and $_[3] -eq $Name
    })
    if ($matches.Count -ne 1) { throw "Expected one $Section/$Key/$Name" }
    return ,$matches[0]
}
function Get-Bytes([string[]]$Row) {
    Assert-Inf ($Row[4] -eq '1') "REG_BINARY for $($Row[3])"
    return ,[byte[]]@($Row[5..($Row.Length-1)] | ForEach-Object { [Convert]::ToByte($_,16) })
}
function U32([byte[]]$Bytes, [int]$Offset) { [BitConverter]::ToUInt32($Bytes,$Offset) }

$reader = [AirModemInfReader]::new($InfPath)
try {
    foreach ($arch in 'NTx86','NTamd64') {
        $models = $reader.Read("AirModem.$arch")
        Assert-Inf ($models.Count -eq 1) "One cable-only model for $arch"
        Assert-Inf ($models[0][2] -eq 'AIRMODEM4800_TRANSPARENT_V1') 'New model identity'
        Assert-Inf ($models[0][1] -eq 'Air4800.Cable') 'Cable install section'
        foreach ($section in 'Air4800.Common','Air4800.Cable.Reg','Air4800.Cable.Responses') {
            foreach ($row in $reader.Read($section)) { Assert-Inf ($row[1] -eq 'HKR') 'Device-relative registry only' }
        }
    }
    $dcb = Get-Bytes (Get-RegRow 'Air4800.Common' '' 'DCB')
    Assert-Inf ($dcb.Length -eq 28 -and (U32 $dcb 0) -eq 28) 'DCB size'
    Assert-Inf ((U32 $dcb 4) -eq 4800) '4800 DTE'
    Assert-Inf ((U32 $dcb 8) -eq 0x381) 'Independent XON/XOFF; no hardware flow'
    Assert-Inf ($dcb[18] -eq 8 -and $dcb[19] -eq 0 -and $dcb[20] -eq 0) '8N1'
    Assert-Inf ($dcb[21] -eq 0x11 -and $dcb[22] -eq 0x13) 'DC1/DC3'
    Assert-Inf ([BitConverter]::ToUInt16($dcb,14) -eq 10 -and [BitConverter]::ToUInt16($dcb,16) -eq 128) 'Flow thresholds'
    $native = [AirModemInfReader]::BuildDcb()
    foreach ($offset in 4,8) { Assert-Inf ((U32 $native $offset) -eq (U32 $dcb $offset)) "Native DCB offset $offset" }
    $device = Get-Bytes (Get-RegRow 'Air4800.Cable.Reg' '' 'DeviceType')
    Assert-Inf ($device.Length -eq 1 -and $device[0] -eq 0) 'Direct serial cable'
    $props = Get-Bytes (Get-RegRow 'Air4800.Cable.Reg' '' 'Properties')
    Assert-Inf ($props.Length -eq 32) 'Property size'
    Assert-Inf ((U32 $props 20) -eq 0x20 -and (U32 $props 24) -eq 4800 -and (U32 $props 28) -eq 4800) 'Capabilities and rates'
    $expected = @{
        'Init/1'='<h11>'; 'Init/2'='NoResponse'
        'Monitor/1'='<h11>'; 'Monitor/2'='None'
        'Answer/1'='CLIENTSERVER'; 'Answer/2'='NoResponse'
        'Settings/DialPrefix'='CLIENT'; 'Settings/FlowControl_Soft'=''
    }
    foreach ($item in $expected.GetEnumerator()) {
        $parts = $item.Key.Split('/')
        $row = Get-RegRow 'Air4800.Cable.Reg' $parts[0] $parts[1]
        Assert-Inf ($row[5] -eq $item.Value) "Cable action $($item.Key)"
    }
    foreach ($row in $reader.Read('Air4800.Cable.Responses')) {
        $bytes = Get-Bytes $row
        Assert-Inf ($bytes.Length -eq 10 -and $bytes[1] -eq 0 -and (U32 $bytes 6) -eq 0) 'Response without baud change'
        if ($row[3] -eq 'CLIENTSERVER') {
            Assert-Inf ($bytes[0] -eq 2 -and (U32 $bytes 2) -eq 4800) 'Connect only on peer response'
        } else { Assert-Inf ($bytes[0] -eq 8) 'CLIENT signals incoming call' }
    }
    $source = Get-Content -LiteralPath $InfPath -Raw
    Assert-Inf ($source -notmatch '(?m)^\s*(CopyFiles|AddService|DefaultInstall|Include|Needs)\s*=') 'No new binary or inherited profile'
    Assert-Inf ($source -notmatch '(?m)^\s*HKR,.*"(AT[^"]*|\+\+\+|OK|CONNECT[^"]*)"') 'No AT commands or local modem response'
    [pscustomobject]@{
        Status='PASS'; Checks=$script:checks; Parser='Windows SetupAPI'
        DcbCheck='Windows BuildCommDCBW'; InfSha256=(Get-FileHash -LiteralPath $InfPath -Algorithm SHA256).Hash
        Baud=4800; Framing='8N1'; DcbFlags='0x00000381'; Models=1
        Signed=$false; InstalledByTest=$false; PortOpened=$false; NativeRasTested=$false
    } | ConvertTo-Json
} finally { $reader.Dispose() }
