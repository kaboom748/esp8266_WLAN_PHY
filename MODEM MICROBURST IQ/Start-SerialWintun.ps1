#requires -Version 5.1
<#
.SYNOPSIS
    Liaison COM / Wintun avec interface graphique, sans saisie en console.
.DESCRIPTION
    Choix obligatoire de wintun.dll dans une boite de fichiers, puis liste COM.
    115200 / 8N1 / XON-XOFF ; trames IP echappees avec FCS-16.
    Ce transport n'est PAS PPP : meme protocole necessaire a l'autre extremite.
    La fenetre reste ouverte. Quitter ou la croix demandent confirmation.
    Apres confirmation : arret du transport, fermeture du COM, fin de session
    et suppression de l'adaptateur cree. Le pilote Wintun reste installe.
    Aucune adresse IP, passerelle, regle de pare-feu ou route globale imposee.
    Lanceur double-clic facultatif : Lancer.vbs. Script PowerShell autonome.
    Windows PowerShell 5.1, Windows 10/11 avec bureau, droits administrateur.
    Aucun changement permanent de strategie d'execution.
    Version 2026-10-31-gui-r4. Prototype non teste sur Windows ni modem reel.
    R4 : lecteur Wintun independant ; file TX bornee ; priorite petits ICMP.
    Filtre IPv4 discret et mode de diagnostic ICMP commutables dans la fenetre.
    Mesures COM/XOFF, deltas d'octets, traces ICMP et export de diagnostic.
.PARAMETER GuiChild
    Parametre interne de relancement : ne pas le saisir.
#>
[CmdletBinding()]
param([switch]$GuiChild)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:AppTitle = 'Liaison série / Wintun'
$script:AdapterName = 'SerialIP'
$script:Mtu = 1500

function Show-AppMessage {
    param([string]$Text, [string]$Title = $script:AppTitle,
          [System.Windows.Forms.MessageBoxIcon]$Icon = [System.Windows.Forms.MessageBoxIcon]::Information)
    [void][System.Windows.Forms.MessageBox]::Show($Text, $Title,
        [System.Windows.Forms.MessageBoxButtons]::OK, $Icon)
}

function New-AppForm {
    param([string]$Title, [int]$Width, [int]$Height)
    $form = [System.Windows.Forms.Form]::new()
    $form.Text = $Title
    $form.Font = [System.Drawing.Font]::new('Segoe UI', 10)
    $form.AutoScaleDimensions = [System.Drawing.SizeF]::new(96, 96)
    $form.AutoScaleMode = [System.Windows.Forms.AutoScaleMode]::Dpi
    $form.ClientSize = [System.Drawing.Size]::new($Width, $Height)
    $form.StartPosition = [System.Windows.Forms.FormStartPosition]::CenterScreen
    $form.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::FixedDialog
    $form.MaximizeBox = $false
    return $form
}

function Add-AppLabel {
    param($Form, [string]$Text, [int]$X, [int]$Y, [int]$Width, [int]$Height)
    $label = [System.Windows.Forms.Label]::new()
    $label.Text = $Text
    $label.SetBounds($X, $Y, $Width, $Height)
    $Form.Controls.Add($label)
    return $label
}

function Test-WintunLibrary {
    param([string]$Path)
    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ($item.PSIsContainer -or $item.Name -ine 'wintun.dll') {
        throw 'Sélectionne le fichier wintun.dll officiel, et non un dossier ou une autre bibliothèque.'
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $item.FullName -ErrorAction Stop
    if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -notmatch 'WireGuard') {
        throw "La signature de cette DLL n'est pas validée comme provenant de WireGuard.`r`nÉtat : $($signature.Status).`r`nChoisis la DLL officielle signée."
    }
    # Lecture du type PE avant tout chargement dans le processus administrateur.
    $stream = $null
    $reader = $null
    try {
        $stream = [IO.File]::Open($item.FullName, [IO.FileMode]::Open,
            [IO.FileAccess]::Read, [IO.FileShare]::Read)
        $reader = [IO.BinaryReader]::new($stream)
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5A4D) {
            throw 'Le fichier sélectionné ne possède pas un en-tête PE Windows valide.'
        }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadUInt32()
        if ($peOffset -gt ($stream.Length - 6)) { throw 'En-tête PE incomplet.' }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw 'Signature PE incorrecte.' }
        $machine = $reader.ReadUInt16()
        $processArch = [Environment]::GetEnvironmentVariable('PROCESSOR_ARCHITECTURE')
        $expected = switch ($processArch) {
            'AMD64' { 0x8664 }
            'ARM64' { 0xAA64 }
            'x86'   { 0x014C }
            default { if ([IntPtr]::Size -eq 8) { 0x8664 } else { 0x014C } }
        }
        if ($machine -ne $expected) {
            throw "Architecture incompatible. Ce PowerShell utilise $processArch ; sélectionne la DLL correspondante dans la distribution Wintun."
        }
    }
    finally {
        if ($null -ne $reader) { $reader.Dispose() }
        elseif ($null -ne $stream) { $stream.Dispose() }
    }
    return $item.FullName
}

function Select-WintunLibrary {
    $picker = [System.Windows.Forms.OpenFileDialog]::new()
    $picker.Title = '1 / 2 — Choisis le fichier wintun.dll'
    $picker.Filter = 'Bibliothèque Wintun (wintun.dll)|wintun.dll'
    $picker.FileName = 'wintun.dll'
    $picker.InitialDirectory = $PSScriptRoot
    $picker.CheckFileExists = $true
    $picker.CheckPathExists = $true
    $picker.Multiselect = $false
    $picker.RestoreDirectory = $true
    try {
        while ($true) {
            if ($picker.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { return $null }
            try { return (Test-WintunLibrary -Path $picker.FileName) }
            catch { Show-AppMessage -Text $_.Exception.Message -Title 'DLL refusée' -Icon Error }
        }
    }
    finally { $picker.Dispose() }
}

function Select-SerialPort {
    param([string]$DllPath)
    $form = New-AppForm -Title '2 / 2 — Choisis le port COM' -Width 680 -Height 348
    $form.MinimizeBox = $false
    $okLabel = Add-AppLabel $form 'DLL trouvée — signature et architecture vérifiées.' 20 18 640 26
    $okLabel.ForeColor = [Drawing.Color]::DarkGreen
    $pathBox = [System.Windows.Forms.TextBox]::new()
    $pathBox.SetBounds(20, 49, 640, 53)
    $pathBox.ReadOnly = $true
    $pathBox.Multiline = $true
    $pathBox.ScrollBars = [System.Windows.Forms.ScrollBars]::Vertical
    $pathBox.Text = $DllPath
    $form.Controls.Add($pathBox)
    $null = Add-AppLabel $form 'Port COM :' 20 128 100 28
    $combo = [System.Windows.Forms.ComboBox]::new()
    $combo.SetBounds(128, 124, 360, 30)
    $combo.DropDownStyle = [System.Windows.Forms.ComboBoxStyle]::DropDownList
    $form.Controls.Add($combo)
    $refresh = [System.Windows.Forms.Button]::new()
    $refresh.SetBounds(504, 122, 156, 32)
    $refresh.Text = 'Actualiser'
    $form.Controls.Add($refresh)
    $portsInfo = Add-AppLabel $form '' 128 165 532 40
    $null = Add-AppLabel $form '115200 bauds  •  8N1  •  XON/XOFF  •  caractères échappés' 20 210 640 26
    $null = Add-AppLabel $form 'Carte temporaire : SerialIP. Les adresses IP restent à ton choix.' 20 239 640 30
    $start = [System.Windows.Forms.Button]::new()
    $start.SetBounds(402, 290, 124, 36)
    $start.Text = 'Démarrer'
    $start.Enabled = $false
    $form.Controls.Add($start)
    $cancel = [System.Windows.Forms.Button]::new()
    $cancel.SetBounds(538, 290, 122, 36)
    $cancel.Text = 'Annuler'
    $cancel.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
    $form.Controls.Add($cancel)
    $form.CancelButton = $cancel
    $form.AcceptButton = $start
    $refreshPorts = {
        try {
            $previous = [string]$combo.SelectedItem
            $combo.Items.Clear()
            $ports = @([System.IO.Ports.SerialPort]::GetPortNames() |
                Where-Object { $_ -match '^COM[1-9][0-9]*$' } |
                ForEach-Object { $_.ToUpperInvariant() } | Sort-Object -Unique |
                Sort-Object { [long]$_.Substring(3) })
            foreach ($portName in $ports) { [void]$combo.Items.Add($portName) }
            if ($combo.Items.Count -gt 0) {
                if ($combo.Items.Contains($previous)) { $combo.SelectedItem = $previous }
                else { $combo.SelectedIndex = 0 }
                $portsInfo.Text = "$($ports.Count) port(s) détecté(s). Sélectionne le port de ton modem."
                $start.Enabled = $true
            }
            else {
                $portsInfo.Text = 'Aucun port détecté. Branche le périphérique, puis clique sur Actualiser.'
                $start.Enabled = $false
            }
        }
        catch {
            $start.Enabled = $false
            $portsInfo.Text = 'Détection des ports impossible.'
            Show-AppMessage -Text $_.Exception.Message -Title 'Ports série' -Icon Error
        }
    }
    $refresh.Add_Click($refreshPorts)
    $start.Add_Click({
        if ($combo.SelectedIndex -ge 0) {
            $form.Tag = [string]$combo.SelectedItem
            $form.DialogResult = [System.Windows.Forms.DialogResult]::OK
            $form.Close()
        }
    })
    try {
        & $refreshPorts
        if ($form.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return [string]$form.Tag }
        return $null
    }
    finally { $form.Dispose() }
}

function Initialize-BridgeInterface {
    param($Bridge)
    $index = [uint32]$Bridge.InterfaceIndex
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        if ($Bridge.LastError) { throw $Bridge.LastError }
        $ipv4 = @(Get-NetIPInterface -InterfaceIndex $index -AddressFamily IPv4 -ErrorAction SilentlyContinue)
        if ($ipv4.Count -gt 0) { break }
        if ([DateTime]::UtcNow -ge $deadline) { throw 'La nouvelle carte est absente du fournisseur réseau Windows.' }
        Start-Sleep -Milliseconds 150
    } while ($true)
    if ($ipv4[0].InterfaceAlias -ne $script:AdapterName) {
        throw 'Le nom de la carte créée ne correspond pas au nom demandé.'
    }
    # Configuration locale active uniquement. Ne pas toucher aux autres interfaces.
    Set-NetIPInterface -InterfaceIndex $index -AddressFamily IPv4 -Dhcp Disabled `
        -DadTransmits 0 -NlMtuBytes $script:Mtu -PolicyStore ActiveStore -ErrorAction Stop
    $ipv6 = @(Get-NetIPInterface -InterfaceIndex $index -AddressFamily IPv6 -ErrorAction SilentlyContinue)
    if ($ipv6.Count -gt 0) {
        Set-NetIPInterface -InterfaceIndex $index -AddressFamily IPv6 -RouterDiscovery Disabled `
            -DadTransmits 0 -NlMtuBytes $script:Mtu -PolicyStore ActiveStore -ErrorAction Stop
    }
    $adapters = @(Get-NetAdapter -IncludeHidden -ErrorAction Stop |
        Where-Object { $_.ifIndex -eq $index })
    if ($adapters.Count -ne 1) { throw 'Identification de la nouvelle carte impossible.' }
    return [string]$adapters[0].InterfaceGuid
}

function Export-BridgeDiagnostic {
    param($Bridge, $Owner)
    $picker = [System.Windows.Forms.SaveFileDialog]::new()
    $picker.Title = 'Enregistrer le diagnostic SerialIP'
    $picker.Filter = 'Rapport texte (*.txt)|*.txt'
    $picker.FileName = 'SerialIP-diagnostic-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.txt'
    $picker.OverwritePrompt = $true
    try {
        if ($picker.ShowDialog($Owner) -ne [System.Windows.Forms.DialogResult]::OK) { return }
        $report = $Bridge.DiagnosticReport()
        $index = [uint32]$Bridge.InterfaceIndex
        $report += "`r`n=== CONFIGURATION SerialIP ===`r`n"
        $report += (Get-NetIPAddress -InterfaceIndex $index -ErrorAction Stop |
            Select-Object IPAddress,PrefixLength,AddressFamily,AddressState,SkipAsSource |
            Format-Table -AutoSize | Out-String -Width 160)
        $report += "`r`n=== ROUTES ACTIVES IPv4 (y compris autres interfaces, pour voir les conflits) ===`r`n"
        $report += (Get-NetRoute -AddressFamily IPv4 -PolicyStore ActiveStore -ErrorAction Stop |
            Select-Object InterfaceIndex,InterfaceAlias,DestinationPrefix,NextHop,RouteMetric |
            Sort-Object DestinationPrefix,InterfaceIndex | Format-Table -AutoSize | Out-String -Width 180)
        $report += "`r`n=== INTERFACE ===`r`n"
        $report += (Get-NetIPInterface -InterfaceIndex $index -ErrorAction Stop |
            Select-Object InterfaceIndex,AddressFamily,NlMtuBytes,InterfaceMetric,Dhcp,ConnectionState |
            Format-Table -AutoSize | Out-String -Width 160)
        [IO.File]::WriteAllText($picker.FileName, $report, [Text.UTF8Encoding]::new($true))
        Show-AppMessage -Text "Diagnostic enregistré.`r`nLe rapport contient des adresses IP et des routes locales ; vérifie-le avant de le partager."
    }
    finally { $picker.Dispose() }
}

function Show-ActiveBridge {
    param($Bridge, [string]$Port, [string]$DllPath)
    $form = New-AppForm -Title 'Liaison série / Wintun — SerialIP — R4' -Width 980 -Height 620
    $title = Add-AppLabel $form 'Démarrage de la liaison…' 20 18 940 36
    $title.Font = [Drawing.Font]::new('Segoe UI', 15, [Drawing.FontStyle]::Bold)
    $stateLabel = Add-AppLabel $form 'Création de la carte et ouverture du port série.' 20 61 940 52
    $details = [System.Windows.Forms.TextBox]::new()
    $details.SetBounds(20, 122, 940, 110)
    $details.Multiline = $true
    $details.ReadOnly = $true
    $details.ScrollBars = [System.Windows.Forms.ScrollBars]::Vertical
    $details.Text = "Port : $Port — 115200 / 8N1 / XON-XOFF`r`nCarte : $($script:AdapterName) — MTU $($script:Mtu)`r`nDLL : $DllPath`r`nTransport IP personnalisé, sans PPP standard ni authentification. R4 : file TX bornée, diagnostic XOFF."
    $form.Controls.Add($details)
    $stats = [System.Windows.Forms.TextBox]::new()
    $stats.SetBounds(20, 245, 940, 194)
    $stats.Multiline = $true
    $stats.ReadOnly = $true
    $stats.ScrollBars = [System.Windows.Forms.ScrollBars]::Both
    $stats.WordWrap = $false
    $stats.Font = [Drawing.Font]::new('Consolas', 9)
    $stats.Text = 'Compteurs en attente…'
    $form.Controls.Add($stats)
    $quietBox = [System.Windows.Forms.CheckBox]::new()
    $quietBox.SetBounds(20, 450, 570, 28)
    $quietBox.Text = 'IPv4 discret : filtrer IPv6, multicast et diffusion avant le COM'
    $quietBox.Checked = $true
    $form.Controls.Add($quietBox)
    $pingBox = [System.Windows.Forms.CheckBox]::new()
    $pingBox.SetBounds(620, 450, 340, 28)
    $pingBox.Text = 'Diagnostic : ICMP (ping) uniquement'
    $form.Controls.Add($pingBox)
    $null = Add-AppLabel $form 'Le mode ICMP bloque TCP/UDP ; décoche-le après le test. Aucun changement global de Windows.' 20 483 940 26
    $null = Add-AppLabel $form 'Les compteurs COM ne prouvent pas la livraison distante. Quitter supprime la carte créée.' 20 513 940 26
    $network = [System.Windows.Forms.Button]::new()
    $network.SetBounds(20, 560, 270, 42)
    $network.Text = 'Ouvrir les connexions réseau'
    $network.Enabled = $false
    $form.Controls.Add($network)
    $quit = [System.Windows.Forms.Button]::new()
    $quit.SetBounds(780, 560, 180, 42)
    $quit.Text = 'Quitter…'
    $form.Controls.Add($quit)
    $export = [System.Windows.Forms.Button]::new()
    $export.SetBounds(305, 560, 250, 42)
    $export.Text = 'Enregistrer le diagnostic…'
    $export.Enabled = $false
    $form.Controls.Add($export)
    $applyFilter = {
        try { $Bridge.SetFilter($quietBox.Checked, $pingBox.Checked) }
        catch { Show-AppMessage -Text $_.Exception.Message -Title 'Filtre' -Icon Error }
    }
    $quietBox.Add_CheckedChanged($applyFilter)
    $pingBox.Add_CheckedChanged($applyFilter)
    $export.Add_Click({
        try { Export-BridgeDiagnostic -Bridge $Bridge -Owner $form }
        catch { Show-AppMessage -Text $_.Exception.Message -Title 'Diagnostic impossible' -Icon Error }
    })
    $timer = [System.Windows.Forms.Timer]::new()
    $timer.Interval = 500
    $state = [pscustomobject]@{
        Phase = 'Start'; AllowClose = $false; Confirming = $false; InTick = $false
        Failure = ''; InterfaceGuid = ''; StopStarted = [DateTime]::MinValue
        VerifyDeadline = [DateTime]::MinValue; CleanupVerified = $false; FinalMessage = ''
    }
    $beginStop = {
        if ($state.Phase -eq 'Stop' -or $state.Phase -eq 'Verify' -or $state.Phase -eq 'Done') { return }
        $state.Phase = 'Stop'
        $state.StopStarted = [DateTime]::UtcNow
        $Bridge.RequestStop()
        $network.Enabled = $false
        $export.Enabled = $false
        $quietBox.Enabled = $false
        $pingBox.Enabled = $false
        $quit.Enabled = $false
        $title.Text = 'Arrêt et nettoyage…'
        $stateLabel.Text = 'Arrêt des transferts, libération du port COM, puis suppression de la carte.'
        $form.UseWaitCursor = $true
    }
    $network.Add_Click({
        try {
            Start-Process -FilePath (Join-Path $env:WINDIR 'System32\control.exe') `
                -ArgumentList 'ncpa.cpl' -ErrorAction Stop
        }
        catch { Show-AppMessage -Text $_.Exception.Message -Title 'Connexions réseau' -Icon Error }
    })
    $quit.Add_Click({ $form.Close() })
    $form.Add_FormClosing({
        param($sender, $eventArgs)
        if ($state.AllowClose) { return }
        # Lors d'un arrêt de Windows, tenter le finally sans bloquer par une confirmation.
        if ($eventArgs.CloseReason -eq [System.Windows.Forms.CloseReason]::WindowsShutDown -or
            $eventArgs.CloseReason -eq [System.Windows.Forms.CloseReason]::TaskManagerClosing) {
            $Bridge.RequestStop()
            $state.AllowClose = $true
            return
        }
        $eventArgs.Cancel = $true
        if ($state.Phase -eq 'Stop' -or $state.Phase -eq 'Verify' -or $state.Confirming) { return }
        $state.Confirming = $true
        try {
            $answer = [System.Windows.Forms.MessageBox]::Show($form,
                "Veux-tu quitter ?`r`n`r`nOui : arrêter la liaison, libérer $Port et supprimer la carte $($script:AdapterName).`r`nNon : laisser la liaison active.",
                'Quitter et supprimer la carte ?', [System.Windows.Forms.MessageBoxButtons]::YesNo,
                [System.Windows.Forms.MessageBoxIcon]::Question,
                [System.Windows.Forms.MessageBoxDefaultButton]::Button2)
            if ($answer -eq [System.Windows.Forms.DialogResult]::Yes) { & $beginStop }
        }
        finally { $state.Confirming = $false }
    })
    $onTick = {
        if ($state.InTick -or $state.Confirming) { return }
        $state.InTick = $true
        try {
            switch ($state.Phase) {
                'Start' {
                    $form.UseWaitCursor = $true
                    $form.Refresh()
                    $Bridge.Start()
                    $state.InterfaceGuid = Initialize-BridgeInterface -Bridge $Bridge
                    if ($Bridge.LastError) { throw $Bridge.LastError }
                    $title.Text = 'Liaison locale active — R4'
                    $title.ForeColor = [Drawing.Color]::DarkGreen
                    $stateLabel.Text = 'DLL chargée. Port ouvert. Carte créée. Le correspondant distant n''est pas vérifié.'
                    $network.Enabled = $true
                    $export.Enabled = $true
                    $form.UseWaitCursor = $false
                    $state.Phase = 'Run'
                }
                'Run' {
                    $stats.Text = $Bridge.Statistics()
                    if ($Bridge.StopRequested) {
                        $state.Failure = [string]$Bridge.LastError
                        if (-not $state.Failure) { $state.Failure = 'Le moteur a demandé l''arrêt de la liaison.' }
                        & $beginStop
                    }
                }
                'Stop' {
                    if ($Bridge.TryCompleteStop()) {
                        $state.Phase = 'Verify'
                        $state.VerifyDeadline = [DateTime]::UtcNow.AddSeconds(10)
                        $stateLabel.Text = 'Port COM libéré. Vérification de la disparition de la carte…'
                    }
                    elseif (([DateTime]::UtcNow - $state.StopStarted).TotalSeconds -gt 10) {
                        $title.ForeColor = [Drawing.Color]::Firebrick
                        $stateLabel.Text = 'Le pilote série tarde à annuler ses entrées/sorties. Nettoyage non terminé : les ressources restent protégées.'
                        $form.UseWaitCursor = $false
                    }
                }
                'Verify' {
                    $remaining = @()
                    if ($Bridge.AdapterWasCreated) {
                        $all = @(Get-NetAdapter -IncludeHidden -ErrorAction Stop)
                        if ($state.InterfaceGuid) {
                            $remaining = @($all | Where-Object { [string]$_.InterfaceGuid -eq $state.InterfaceGuid })
                        }
                        else {
                            $remaining = @($all | Where-Object { $_.Name -eq $script:AdapterName })
                        }
                    }
                    if ($remaining.Count -eq 0) {
                        $state.CleanupVerified = $true
                        $state.FinalMessage = if ($Bridge.AdapterWasCreated) {
                            "Liaison arrêtée.`r`nPort $Port libéré.`r`nCarte $($script:AdapterName) supprimée.`r`nLe pilote Wintun reste installé."
                        } else { 'Aucune carte créée. Les ressources ouvertes ont été libérées.' }
                        $state.Phase = 'Done'
                    }
                    elseif ([DateTime]::UtcNow -ge $state.VerifyDeadline) {
                        $state.FinalMessage = 'Le port est libéré et WintunCloseAdapter a été appelé, mais Windows affiche encore la carte. La suppression n''est pas confirmée.'
                        $state.Phase = 'Done'
                    }
                }
                'Done' {
                    $timer.Stop()
                    $state.AllowClose = $true
                    $form.Close()
                }
            }
        }
        catch {
            $message = $_.Exception.Message
            if (-not $state.Failure) { $state.Failure = $message }
            if ($state.Phase -eq 'Verify') {
                $state.FinalMessage = 'Les handles ont été fermés, mais la vérification de suppression de la carte a échoué.'
                $state.Phase = 'Done'
            }
            elseif ($state.Phase -eq 'Stop') {
                # Ne pas annoncer un nettoyage réussi en cas d'erreur native.
                $state.FinalMessage = 'Le nettoyage a rencontré une erreur. La suppression de la carte n''est pas confirmée.'
                $timer.Stop()
                $state.AllowClose = $true
                $form.Close()
            }
            else { & $beginStop }
        }
        finally { $state.InTick = $false }
    }
    $timer.Add_Tick($onTick)
    $form.Add_Shown({ $timer.Start() })
    try { [void]$form.ShowDialog() }
    finally {
        $timer.Stop()
        $timer.Dispose()
        try { if (-not $Bridge.IsDisposed) { $Bridge.Dispose() } }
        finally { $form.Dispose() }
    }
    if ($state.FinalMessage) {
        if ($state.Failure) {
            Show-AppMessage -Text ($state.Failure + "`r`n`r`n" + $state.FinalMessage) -Title 'Liaison interrompue' -Icon Error
        }
        elseif (-not $state.CleanupVerified) {
            Show-AppMessage -Text $state.FinalMessage -Title 'Nettoyage à vérifier' -Icon Warning
        }
        else { Show-AppMessage -Text $state.FinalMessage -Title 'Liaison fermée' }
    }
}

# Moteur intégré : les transferts utilisent des threads C#, sans appeler PowerShell
# depuis ces threads. La boucle graphique reste sur son thread STA.
$source = @'
using System;
using System.ComponentModel;
using System.Collections.Generic;
using System.Diagnostics;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

namespace SerialWintunV1
{
    // Original framing implementation. This is NOT a PPP implementation.
    public static class Framing
    {
        public static ushort UpdateFcs(ushort fcs, byte value)
        {
            int v = fcs ^ value;
            for (int bit = 0; bit < 8; bit++)
                v = (v & 1) != 0 ? (v >> 1) ^ 0x8408 : v >> 1;
            return (ushort)v;
        }

        public static bool MustEscape(byte value)
        {
            return value < 0x20 || value == 0x7D || value == 0x7E ||
                   value == 0x91 || value == 0x93;
        }

        private static void Append(byte[] output, ref int index, byte value)
        {
            if (MustEscape(value))
            {
                output[index++] = 0x7D;
                output[index++] = (byte)(value ^ 0x20);
            }
            else output[index++] = value;
        }

        public static byte[] Encode(byte[] packet, int count)
        {
            if (packet == null || count < 0 || count > packet.Length || count > 65535)
                throw new ArgumentException("Invalid packet length.");
            byte[] encoded = new byte[2 * (count + 2) + 2];
            int index = 0;
            encoded[index++] = 0x7E;
            ushort fcs = 0xFFFF;
            for (int i = 0; i < count; i++)
            {
                fcs = UpdateFcs(fcs, packet[i]);
                Append(encoded, ref index, packet[i]);
            }
            fcs ^= 0xFFFF;
            Append(encoded, ref index, (byte)(fcs & 0xFF));
            Append(encoded, ref index, (byte)(fcs >> 8));
            encoded[index++] = 0x7E;
            Array.Resize(ref encoded, index);
            return encoded;
        }

        public static bool IsIpPacket(byte[] packet, int count, int mtu)
        {
            if (packet == null || count < 20 || count > packet.Length || count > mtu)
                return false;
            int version = packet[0] >> 4;
            if (version == 4)
            {
                int headerLength = (packet[0] & 15) * 4;
                int totalLength = (packet[2] << 8) | packet[3];
                return headerLength >= 20 && headerLength <= count && totalLength == count;
            }
            if (version == 6 && count >= 40)
            {
                int payloadLength = (packet[4] << 8) | packet[5];
                return payloadLength + 40 == count; // No IPv6 jumbograms.
            }
            return false;
        }
    }

    public sealed class Decoder
    {
        private readonly byte[] buffer;
        private readonly Action<byte[], int> deliver;
        private int length;
        private bool inFrame, escaped, discard;
        private long badFrames;
        public long BadFrames { get { return Interlocked.Read(ref badFrames); } }

        public Decoder(int maxPayload, Action<byte[], int> callback)
        {
            if (maxPayload < 0 || maxPayload > 65535 || callback == null)
                throw new ArgumentException("Invalid decoder parameters.");
            buffer = new byte[maxPayload + 2];
            deliver = callback;
        }

        public void Push(byte[] bytes, int count)
        {
            if (bytes == null || count < 0 || count > bytes.Length)
                throw new ArgumentException("Invalid input length.");
            for (int i = 0; i < count; i++) PushByte(bytes[i]);
        }

        public void PushByte(byte value)
        {
            if (value == 0x7E)
            {
                if (inFrame && !discard && (length != 0 || escaped))
                {
                    ushort fcs = 0xFFFF;
                    for (int i = 0; i < length; i++) fcs = Framing.UpdateFcs(fcs, buffer[i]);
                    if (!escaped && length >= 2 && fcs == 0xF0B8)
                        deliver(buffer, length - 2);
                    else Interlocked.Increment(ref badFrames);
                }
                inFrame = true;
                length = 0;
                escaped = false;
                discard = false;
                return;
            }
            if (!inFrame || discard) return;
            // Fixed receive ACCM: raw control characters are not payload.
            // This also tolerates XON/XOFF inserted between escape bytes.
            if (value < 0x20) return;
            if (escaped)
            {
                value ^= 0x20;
                escaped = false;
            }
            else if (value == 0x7D)
            {
                escaped = true;
                return;
            }
            if (length == buffer.Length)
            {
                discard = true;
                Interlocked.Increment(ref badFrames);
                return;
            }
            buffer[length++] = value;
        }
    }

    // Classification only; this is NOT a firewall and provides no authentication.
    public static class PacketPolicy
    {
        public static uint Address(byte[] p, int offset)
        {
            return ((uint)p[offset] << 24) | ((uint)p[offset + 1] << 16) |
                   ((uint)p[offset + 2] << 8) | p[offset + 3];
        }
        public static string AddressText(byte[] p, int offset)
        {
            return p[offset] + "." + p[offset + 1] + "." + p[offset + 2] + "." + p[offset + 3];
        }
        // Return a short reason, or null when the packet is allowed.
        public static string Reject(byte[] p, int count, bool quiet, bool pingOnly, uint[] broadcasts)
        {
            int version = p[0] >> 4;
            if (!quiet && !pingOnly) return null;
            if (version != 4) return "IPv6";
            uint dst = Address(p, 16);
            if ((dst & 0xF0000000u) == 0xE0000000u) return "multicast";
            if (dst == UInt32.MaxValue) return "diffusion";
            if (broadcasts != null)
                foreach (uint value in broadcasts) if (value == dst) return "diffusion";
            if (pingOnly) return p[9] == 1 ? null : "hors-ICMP";
            if (p[9] == 2) return "IGMP";
            // No UDP-port blacklist: normal unicast services are preserved.
            return null;
        }
        public static bool Priority(byte[] p, int count)
        {
            // Keep normal FIFO order within each class. No preemption of a frame already on COM.
            return count <= 512 && (p[0] >> 4) == 4 && p[9] == 1;
        }
        public static string Icmp(byte[] p, int count)
        {
            if (count < 28 || (p[0] >> 4) != 4 || p[9] != 1) return null;
            int h = (p[0] & 15) * 4;
            if (h < 20 || count < h + 8 || ((p[6] & 0x1F) != 0) || p[7] != 0) return null;
            int type = p[h];
            return AddressText(p,12) + " -> " + AddressText(p,16) + " type=" + type +
                " id=" + ((p[h+4] << 8) | p[h+5]) + " seq=" + ((p[h+6] << 8) | p[h+7]);
        }
        public static int EchoType(byte[] p, int count)
        {
            if (count < 28 || (p[0] >> 4) != 4 || p[9] != 1) return -1;
            int h = (p[0] & 15) * 4;
            if (h < 20 || count < h + 8 || (p[6] & 0x1F) != 0 || p[7] != 0) return -1;
            return p[h] == 8 ? 8 : p[h] == 0 ? 0 : -1;
        }
    }

    internal sealed class QueuedPacket
    {
        internal byte[] Ip, Wire;
        internal long Created;
        internal bool High;
    }

    // Bounded *escaped wire bytes*, not just number of packets.
    // Drops are whole packets, never fragments of a serial frame.
    internal sealed class PacketQueue
    {
        private readonly Queue<QueuedPacket> high = new Queue<QueuedPacket>();
        private readonly Queue<QueuedPacket> normal = new Queue<QueuedPacket>();
        private readonly object gate = new object();
        private readonly int maximum;
        private readonly long lifetime;
        private int bytes, highBurst;
        private long dropped, expired;
        internal PacketQueue(int maximumBytes, long lifetimeTicks)
        {
            maximum = maximumBytes;
            lifetime = lifetimeTicks;
        }
        private void Expire(Queue<QueuedPacket> q, long now)
        {
            while (q.Count > 0 && now - q.Peek().Created > lifetime)
            {
                bytes -= q.Dequeue().Wire.Length;
                expired++;
            }
        }
        private void Prune(long now) { Expire(high, now); Expire(normal, now); }
        internal bool Put(QueuedPacket packet, long now)
        {
            lock (gate)
            {
                Prune(now);
                if (packet.Wire.Length > maximum) { dropped++; return false; }
                // High-priority packets may replace older NORMAL packets, never an in-flight frame.
                while (bytes + packet.Wire.Length > maximum && packet.High && normal.Count > 0)
                {
                    bytes -= normal.Dequeue().Wire.Length;
                    dropped++;
                }
                if (bytes + packet.Wire.Length > maximum) { dropped++; return false; }
                (packet.High ? high : normal).Enqueue(packet);
                bytes += packet.Wire.Length;
                return true;
            }
        }
        internal QueuedPacket Take(long now)
        {
            lock (gate)
            {
                Prune(now);
                QueuedPacket packet;
                // At most 8 consecutive priority packets when normal traffic is also waiting.
                if (high.Count > 0 && (highBurst < 8 || normal.Count == 0))
                { packet = high.Dequeue(); highBurst++; }
                else if (normal.Count > 0) { packet = normal.Dequeue(); highBurst = 0; }
                else return null;
                bytes -= packet.Wire.Length;
                return packet;
            }
        }
        internal void Clear()
        {
            lock (gate)
            {
                dropped += high.Count + normal.Count;
                high.Clear(); normal.Clear(); bytes = 0; highBurst = 0;
            }
        }
        internal string Snapshot(long now)
        {
            lock (gate)
            {
                Prune(now);
                long oldest = now;
                if (high.Count > 0) oldest = Math.Min(oldest, high.Peek().Created);
                if (normal.Count > 0) oldest = Math.Min(oldest, normal.Peek().Created);
                return String.Format("File logiciel : {0} p / {1}/{2} o echappes ; age max {3:F1} s ; debord. {4}, perimes {5}",
                    high.Count + normal.Count, bytes, maximum,
                    (now - oldest) / (double)Stopwatch.Frequency, dropped, expired);
            }
        }
        internal int Bytes { get { lock (gate) return bytes; } }
        internal long Dropped { get { lock (gate) return dropped; } }
        internal long Expired { get { lock (gate) return expired; } }
    }

    // Some structure fields are populated by the Win32 marshaller.
#pragma warning disable 0649
    internal static class Native
    {
        internal const uint Infinite = 0xFFFFFFFF;
        internal const int ErrorPending = 997;
        internal const int ErrorAborted = 995;
        internal const int ErrorNoMoreItems = 259;
        internal const int ErrorBufferOverflow = 111;
        internal static readonly IntPtr InvalidHandle = new IntPtr(-1);

        [StructLayout(LayoutKind.Sequential)]
        internal struct Dcb
        {
            internal uint Length, BaudRate, Flags;
            internal ushort Reserved, XonLim, XoffLim;
            internal byte ByteSize, Parity, StopBits, XonChar, XoffChar, ErrorChar, EofChar, EvtChar;
            internal ushort Reserved1;
        }
        [StructLayout(LayoutKind.Sequential)]
        internal struct ComStat
        {
            internal uint Flags, InQueue, OutQueue;
        }
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool ClearCommError(IntPtr handle, out uint errors, out ComStat status);

        [StructLayout(LayoutKind.Sequential)]
        internal struct Timeouts
        {
            internal uint ReadInterval, ReadMultiplier, ReadConstant, WriteMultiplier, WriteConstant;
        }
        [StructLayout(LayoutKind.Sequential)]
        internal struct Overlapped
        {
            internal UIntPtr Internal, InternalHigh;
            internal uint Offset, OffsetHigh;
            internal IntPtr Event;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
        internal static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true, ExactSpelling = true)]
        internal static extern IntPtr GetProcAddress(IntPtr module, string name);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool FreeLibrary(IntPtr module);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
        internal static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr security,
                                                  uint creation, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GetCommState(IntPtr handle, ref Dcb dcb);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetCommState(IntPtr handle, ref Dcb dcb);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetupComm(IntPtr handle, uint inputSize, uint outputSize);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GetCommTimeouts(IntPtr handle, out Timeouts timeouts);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetCommTimeouts(IntPtr handle, ref Timeouts timeouts);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool PurgeComm(IntPtr handle, uint flags);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool ReadFile(IntPtr handle, IntPtr buffer, uint length, IntPtr count, IntPtr overlapped);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool WriteFile(IntPtr handle, IntPtr buffer, uint length, IntPtr count, IntPtr overlapped);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GetOverlappedResult(IntPtr handle, IntPtr overlapped, out uint count,
                                                        [MarshalAs(UnmanagedType.Bool)] bool wait);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool CancelIoEx(IntPtr handle, IntPtr overlapped);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
        internal static extern IntPtr CreateEventW(IntPtr attributes,
            [MarshalAs(UnmanagedType.Bool)] bool manualReset,
            [MarshalAs(UnmanagedType.Bool)] bool initialState, string name);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool ResetEvent(IntPtr handle);
        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern uint WaitForMultipleObjects(uint count, IntPtr[] handles,
            [MarshalAs(UnmanagedType.Bool)] bool waitAll, uint milliseconds);
        [DllImport("iphlpapi.dll", ExactSpelling = true)]
        internal static extern uint ConvertInterfaceLuidToIndex(ref ulong luid, out uint index);

        internal static Win32Exception Error(string operation)
        {
            return Error(operation, Marshal.GetLastWin32Error());
        }
        internal static Win32Exception Error(string operation, int error)
        {
            return new Win32Exception(error, operation + ": " + new Win32Exception(error).Message +
                                               " (Win32 " + error + ")");
        }
    }

#pragma warning restore 0649

    internal sealed class WintunApi : IDisposable
    {
        private IntPtr module;
        [UnmanagedFunctionPointer(CallingConvention.Winapi, CharSet = CharSet.Unicode, SetLastError = true)]
        internal delegate IntPtr CreateDelegate(string name, string tunnelType, ref Guid requestedGuid);
        [UnmanagedFunctionPointer(CallingConvention.Winapi, CharSet = CharSet.Unicode, SetLastError = true)]
        internal delegate IntPtr OpenDelegate(string name);
        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        internal delegate void CloseDelegate(IntPtr handle);
        [UnmanagedFunctionPointer(CallingConvention.Winapi, SetLastError = true)]
        internal delegate IntPtr StartDelegate(IntPtr adapter, uint capacity);
        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        internal delegate void LuidDelegate(IntPtr adapter, out ulong luid);
        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        internal delegate IntPtr EventDelegate(IntPtr session);
        [UnmanagedFunctionPointer(CallingConvention.Winapi, SetLastError = true)]
        internal delegate IntPtr ReceiveDelegate(IntPtr session, out uint size);
        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        internal delegate void PacketDelegate(IntPtr session, IntPtr packet);
        [UnmanagedFunctionPointer(CallingConvention.Winapi, SetLastError = true)]
        internal delegate IntPtr AllocateDelegate(IntPtr session, uint size);

        internal CreateDelegate Create;
        internal OpenDelegate Open;
        internal CloseDelegate Close, End;
        internal StartDelegate Start;
        internal LuidDelegate GetLuid;
        internal EventDelegate GetReadEvent;
        internal ReceiveDelegate Receive;
        internal PacketDelegate Release, Send;
        internal AllocateDelegate Allocate;

        private T Resolve<T>(string name) where T : class
        {
            IntPtr address = Native.GetProcAddress(module, name);
            if (address == IntPtr.Zero) throw Native.Error("Export missing: " + name);
            return (T)(object)Marshal.GetDelegateForFunctionPointer(address, typeof(T));
        }
        internal WintunApi(string path)
        {
            // Absolute filename + DLL's own directory + System32; not the current directory/PATH.
            module = Native.LoadLibraryExW(Path.GetFullPath(path), IntPtr.Zero, 0x00000900);
            if (module == IntPtr.Zero) throw Native.Error("LoadLibraryExW(wintun.dll); check DLL architecture");
            try
            {
                Create = Resolve<CreateDelegate>("WintunCreateAdapter");
                Open = Resolve<OpenDelegate>("WintunOpenAdapter");
                Close = Resolve<CloseDelegate>("WintunCloseAdapter");
                Start = Resolve<StartDelegate>("WintunStartSession");
                End = Resolve<CloseDelegate>("WintunEndSession");
                GetLuid = Resolve<LuidDelegate>("WintunGetAdapterLUID");
                GetReadEvent = Resolve<EventDelegate>("WintunGetReadWaitEvent");
                Receive = Resolve<ReceiveDelegate>("WintunReceivePacket");
                Release = Resolve<PacketDelegate>("WintunReleaseReceivePacket");
                Allocate = Resolve<AllocateDelegate>("WintunAllocateSendPacket");
                Send = Resolve<PacketDelegate>("WintunSendPacket");
            }
            catch { Dispose(); throw; }
        }
        public void Dispose()
        {
            if (module == IntPtr.Zero) return;
            Native.FreeLibrary(module);
            module = IntPtr.Zero;
        }
    }

    internal sealed class SerialDevice : IDisposable
    {
        internal IntPtr Handle = Native.InvalidHandle;
        private Native.Dcb previousDcb;
        private Native.Timeouts previousTimeouts;
        private bool haveDcb, haveTimeouts;

        internal SerialDevice(string port, bool dtr, bool rts)
        {
            Handle = Native.CreateFileW(@"\\.\" + port, 0xC0000000, 0, IntPtr.Zero,
                                        3, 0x40000000, IntPtr.Zero);
            if (Handle == Native.InvalidHandle) throw Native.Error("Open " + port);
            try
            {
                previousDcb.Length = (uint)Marshal.SizeOf(typeof(Native.Dcb));
                if (!Native.GetCommState(Handle, ref previousDcb)) throw Native.Error("GetCommState");
                haveDcb = true;
                if (!Native.GetCommTimeouts(Handle, out previousTimeouts)) throw Native.Error("GetCommTimeouts");
                haveTimeouts = true;
                if (!Native.SetupComm(Handle, 16384, 4096)) throw Native.Error("SetupComm");
                Native.Dcb configuration = new Native.Dcb();
                configuration.Length = (uint)Marshal.SizeOf(typeof(Native.Dcb));
                configuration.BaudRate = 115200;
                // Binary; TX continues after WE send XOFF; respect received XOFF; generate XON/XOFF.
                // NO CTS/DSR hardware gating, NO DSR sensitivity, NO stripping NUL bytes.
                configuration.Flags = 1u | (1u << 7) | (1u << 8) | (1u << 9);
                if (dtr) configuration.Flags |= 1u << 4;  // Asserted, not handshake.
                if (rts) configuration.Flags |= 1u << 12; // Asserted, not handshake.
                configuration.XonLim = 512;
                configuration.XoffLim = 1024;
                configuration.ByteSize = 8;
                configuration.Parity = 0;
                configuration.StopBits = 0;
                configuration.XonChar = 0x11;
                configuration.XoffChar = 0x13;
                if (!Native.SetCommState(Handle, ref configuration)) throw Native.Error("SetCommState 115200/8N1/XON-XOFF");
                Native.Dcb check = new Native.Dcb();
                check.Length = configuration.Length;
                if (!Native.GetCommState(Handle, ref check)) throw Native.Error("Verify GetCommState");
                if (check.BaudRate != 115200 || check.ByteSize != 8 || check.Parity != 0 ||
                    check.StopBits != 0 || check.XonChar != 0x11 || check.XoffChar != 0x13 ||
                    (check.Flags & 0x7FFF) != configuration.Flags)
                    throw new IOException("Serial driver did not retain the requested 115200/8N1/XON-XOFF settings.");
                Native.Timeouts timeouts = new Native.Timeouts();
                timeouts.ReadInterval = UInt32.MaxValue;
                timeouts.ReadMultiplier = UInt32.MaxValue;
                timeouts.ReadConstant = 200;
                // No write deadline: XOFF is a legitimate pause. OVERLAPPED I/O is cancellable.
                if (!Native.SetCommTimeouts(Handle, ref timeouts)) throw Native.Error("SetCommTimeouts");
                if (!Native.PurgeComm(Handle, 0x000C)) throw Native.Error("PurgeComm at startup");
            }
            catch { Dispose(); throw; }
        }
        public void Dispose()
        {
            if (Handle == Native.InvalidHandle) return;
            // Called ONLY after both worker threads have completed all pending I/O.
            Native.PurgeComm(Handle, 0x000F);
            if (haveDcb) Native.SetCommState(Handle, ref previousDcb);
            if (haveTimeouts) Native.SetCommTimeouts(Handle, ref previousTimeouts);
            Native.CloseHandle(Handle);
            Handle = Native.InvalidHandle;
        }
    }

    // One instance per worker: its unmanaged buffers stay alive until I/O completion.
    internal sealed class SerialIo : IDisposable
    {
        private readonly IntPtr serial;
        private readonly IntPtr stop;
        private IntPtr completed, overlapped;
        internal IntPtr Buffer;
        private readonly int capacity;
        private bool pending;

        internal SerialIo(IntPtr serialHandle, IntPtr stopHandle, int bufferSize)
        {
            serial = serialHandle;
            stop = stopHandle;
            capacity = bufferSize;
            try
            {
                completed = Native.CreateEventW(IntPtr.Zero, true, false, null);
                if (completed == IntPtr.Zero) throw Native.Error("CreateEventW");
                Buffer = Marshal.AllocHGlobal(bufferSize);
                overlapped = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(Native.Overlapped)));
            }
            catch { Dispose(); throw; }
        }

        internal int Transfer(bool write, int offset, int count)
        {
            if (offset < 0 || count < 0 || offset + count > capacity)
                throw new ArgumentOutOfRangeException("count");
            if (!Native.ResetEvent(completed)) throw Native.Error("ResetEvent");
            Native.Overlapped operation = new Native.Overlapped();
            operation.Event = completed;
            Marshal.StructureToPtr(operation, overlapped, false);
            IntPtr address = IntPtr.Add(Buffer, offset);
            bool immediate = write
                ? Native.WriteFile(serial, address, (uint)count, IntPtr.Zero, overlapped)
                : Native.ReadFile(serial, address, (uint)count, IntPtr.Zero, overlapped);
            if (!immediate)
            {
                int error = Marshal.GetLastWin32Error();
                if (error == Native.ErrorAborted) throw new OperationCanceledException();
                if (error != Native.ErrorPending) throw Native.Error(write ? "WriteFile" : "ReadFile", error);
                pending = true;
            }
            try
            {
                if (pending)
                {
                    uint wait = Native.WaitForMultipleObjects(2, new IntPtr[] { stop, completed }, false, Native.Infinite);
                    if (wait == 0) throw new OperationCanceledException();
                    if (wait != 1) throw Native.Error("Wait for serial I/O");
                }
                uint transferred;
                bool success = Native.GetOverlappedResult(serial, overlapped, out transferred, false);
                int error = success ? 0 : Marshal.GetLastWin32Error();
                // A signalled manual-reset event means the OVERLAPPED operation has completed.
                if (error != 996) pending = false; // ERROR_IO_INCOMPLETE must still be drained.
                if (!success)
                {
                    if (error == Native.ErrorAborted) throw new OperationCanceledException();
                    throw Native.Error("GetOverlappedResult", error);
                }
                return checked((int)transferred);
            }
            finally { DrainCancelledOperation(); }
        }

        private void DrainCancelledOperation()
        {
            if (!pending) return;
            Native.CancelIoEx(serial, overlapped);
            uint ignored;
            // NEVER free/reuse OVERLAPPED or its buffer before the driver completes it.
            Native.GetOverlappedResult(serial, overlapped, out ignored, true);
            pending = false;
        }
        public void Dispose()
        {
            DrainCancelledOperation();
            if (overlapped != IntPtr.Zero) { Marshal.FreeHGlobal(overlapped); overlapped = IntPtr.Zero; }
            if (Buffer != IntPtr.Zero) { Marshal.FreeHGlobal(Buffer); Buffer = IntPtr.Zero; }
            if (completed != IntPtr.Zero) { Native.CloseHandle(completed); completed = IntPtr.Zero; }
        }
    }

    public sealed class Bridge : IDisposable
    {
        public const string BuildId = "2026-10-31-gui-r4";
        public bool IsDisposed { get { return disposed; } }
        public bool AdapterWasCreated { get; private set; }
        private readonly string dllPath, port, adapterName;
        private readonly int mtu;
        private readonly bool dtr, rts;
        private readonly ManualResetEvent stop = new ManualResetEvent(false);
        private readonly object errorLock = new object();
        private Exception error;
        private WintunApi api;
        private SerialDevice serial;
        private IntPtr adapter, session, readEvent;
        private Thread serialReader, serialWriter, wintunReader;
        private Decoder decoder;
        private Mutex ownerMutex;
        private bool ownMutex, started, disposed;
        private long txPackets, rxPackets, txBytes, rxBytes, txDropped, rxDropped;
        private long txWire, rxWire, seenPackets, txFiltered, rxFiltered;
        private long seenIpv6, seenIcmp, seenTcp, seenUdp;
        private long seenEchoRequest, seenEchoReply, txEchoRequest, txEchoReply;
        private long rxEchoRequest, rxEchoReply, injectEchoRequest, injectEchoReply;
        private long writingSince, lastAddressPoll;
        private readonly PacketQueue pendingPackets = new PacketQueue(8192, 3 * Stopwatch.Frequency);
        private readonly AutoResetEvent queueReady = new AutoResetEvent(false);
        private readonly object traceLock = new object();
        private readonly Queue<string> traces = new Queue<string>();
        private volatile bool quiet = true, pingOnly;
        private volatile uint[] broadcasts = new uint[0];
        private string addressText = "aucune IPv4 detectee";
        private uint lastComFlags, comIn, comOut, cumulativeErrors;
        private bool comStatusOk, xoffHeld;
        private int comStatusError;
        private long xoffStart, lastRateTick, previousTxWire, previousRxWire;
        private double txRate, rxRate;
        public uint InterfaceIndex { get; private set; }
        public bool StopRequested { get { return stop.WaitOne(0); } }
        public string LastError { get { lock (errorLock) return error == null ? null : error.ToString(); } }

        public Bridge(string libraryPath, string comPort, string name, int maximumPacket, bool enableDtr, bool enableRts)
        {
            if (String.IsNullOrWhiteSpace(name) || name.Length > 64 || maximumPacket < 1280 || maximumPacket > 9000)
                throw new ArgumentException("Invalid adapter name or MTU.");
            dllPath = libraryPath;
            port = comPort;
            adapterName = name;
            mtu = maximumPacket;
            dtr = enableDtr;
            rts = enableRts;
        }

        private static Guid StableGuid(string name)
        {
            byte[] hash;
            using (SHA256 algorithm = SHA256.Create())
                hash = algorithm.ComputeHash(Encoding.UTF8.GetBytes("SerialWintunV1:" + name.ToUpperInvariant()));
            byte[] bytes = new byte[16];
            Array.Copy(hash, bytes, 16);
            return new Guid(bytes);
        }

        public void Start()
        {
            if (disposed || started) throw new InvalidOperationException("Bridge cannot be restarted.");
            started = true;
            try
            {
                Guid guid = StableGuid(adapterName);
                ownerMutex = new Mutex(false, "Global\\SerialWintunV1-" + guid.ToString("N"));
                try { ownMutex = ownerMutex.WaitOne(0); }
                catch (AbandonedMutexException) { ownMutex = true; }
                if (!ownMutex) throw new IOException("Another instance owns this adapter name.");
                api = new WintunApi(dllPath);
                IntPtr existing = api.Open(adapterName);
                if (existing != IntPtr.Zero)
                {
                    api.Close(existing); // Opening/closing an existing adapter does NOT remove it.
                    throw new IOException("Adapter already exists; it will not be reused or deleted: " + adapterName);
                }
                serial = new SerialDevice(port, dtr, rts);
                adapter = api.Create(adapterName, "SerialIP", ref guid);
                if (adapter == IntPtr.Zero) throw Native.Error("WintunCreateAdapter");
                AdapterWasCreated = true;
                ulong luid;
                api.GetLuid(adapter, out luid);
                uint index;
                uint result = Native.ConvertInterfaceLuidToIndex(ref luid, out index);
                if (result != 0) throw Native.Error("ConvertInterfaceLuidToIndex", (int)result);
                InterfaceIndex = index;
                session = api.Start(adapter, 0x20000); // Minimum 128 KiB ring; bounded buffering.
                if (session == IntPtr.Zero) throw Native.Error("WintunStartSession");
                readEvent = api.GetReadEvent(session); // Borrowed handle: do not close it.
                if (readEvent == IntPtr.Zero) throw new IOException("Wintun returned no read event.");
                decoder = new Decoder(mtu, InjectIpPacket);
                wintunReader = new Thread(ReadWintun);
                wintunReader.IsBackground = true;
                wintunReader.Name = "SerialWintun Wintun-pump";
                serialReader = new Thread(ReadSerial);
                serialWriter = new Thread(WriteSerial);
                serialReader.IsBackground = serialWriter.IsBackground = true;
                serialReader.Name = "SerialWintun RX";
                serialWriter.Name = "SerialWintun TX";
                serialReader.Start();
                wintunReader.Start();
                serialWriter.Start();
                Note("Demarrage R4 : file TX 8192 octets echappes, expiration 3 s, filtre IPv4 discret actif.");
            }
            catch { RequestStop(); throw; }
        }

        private void Fail(Exception exception)
        {
            lock (errorLock) { if (error == null) error = exception; }
            stop.Set();
        }
        private void InjectIpPacket(byte[] packet, int count)
        {
            if (StopRequested) return;
            if (!Framing.IsIpPacket(packet, count, mtu)) { Interlocked.Increment(ref rxDropped); return; }
            CountEcho(packet, count, ref rxEchoRequest, ref rxEchoReply);
            TraceIcmp("RX-DECODE-COM", packet, count);
            if (Filter(packet, count) != null) { Interlocked.Increment(ref rxFiltered); return; }
            IntPtr target = api.Allocate(session, (uint)count);
            if (target == IntPtr.Zero)
            {
                int code = Marshal.GetLastWin32Error();
                if (code == Native.ErrorBufferOverflow) { Interlocked.Increment(ref rxDropped); return; }
                throw Native.Error("WintunAllocateSendPacket", code);
            }
            Marshal.Copy(packet, 0, target, count);
            api.Send(session, target); // Inject received IP into the Windows network stack.
            CountEcho(packet, count, ref injectEchoRequest, ref injectEchoReply);
            TraceIcmp("RX-INJECTE-WINTUN", packet, count);
            Interlocked.Increment(ref rxPackets);
            Interlocked.Add(ref rxBytes, count);
        }
        private void ReadSerial()
        {
            try
            {
                using (SerialIo io = new SerialIo(serial.Handle, stop.SafeWaitHandle.DangerousGetHandle(), 4096))
                {
                    byte[] bytes = new byte[4096];
                    while (!StopRequested)
                    {
                        int count = io.Transfer(false, 0, bytes.Length);
                        if (count == 0) continue;
                        Marshal.Copy(io.Buffer, bytes, 0, count);
                        Interlocked.Add(ref rxWire, count);
                        decoder.Push(bytes, count);
                    }
                }
            }
            catch (OperationCanceledException ex) { if (!StopRequested) Fail(ex); }
            catch (Exception ex) { Fail(ex); }
        }
        private void WriteSerial()
        {
            try
            {
                IntPtr stopHandle = stop.SafeWaitHandle.DangerousGetHandle();
                IntPtr readyHandle = queueReady.SafeWaitHandle.DangerousGetHandle();
                using (SerialIo io = new SerialIo(serial.Handle, stopHandle, 2 * (mtu + 2) + 2))
                {
                    while (!StopRequested)
                    {
                        QueuedPacket next = pendingPackets.Take(Stopwatch.GetTimestamp());
                        if (next == null)
                        {
                            uint wait = Native.WaitForMultipleObjects(2, new IntPtr[] { stopHandle, readyHandle }, false, 250);
                            if (wait == 0) return;
                            if (wait != 1 && wait != 258) throw Native.Error("Wait for software queue");
                            continue;
                        }
                        if (Filter(next.Ip, next.Ip.Length) != null)
                        { Interlocked.Increment(ref txFiltered); continue; }
                        if (StopRequested) return;
                        Marshal.Copy(next.Wire, 0, io.Buffer, next.Wire.Length);
                        int offset = 0;
                        Interlocked.Exchange(ref writingSince, Stopwatch.GetTimestamp());
                        TraceIcmp("TX-DEBUT-COM", next.Ip, next.Ip.Length);
                        try
                        {
                            while (offset < next.Wire.Length && !StopRequested)
                            {
                                int count = io.Transfer(true, offset, next.Wire.Length - offset);
                                if (count <= 0) throw new IOException("Serial write made no progress.");
                                offset += count;
                                Interlocked.Add(ref txWire, count);
                            }
                            if (offset == next.Wire.Length)
                            {
                                Interlocked.Increment(ref txPackets);
                                Interlocked.Add(ref txBytes, next.Ip.Length);
                                CountEcho(next.Ip, next.Ip.Length, ref txEchoRequest, ref txEchoReply);
                                TraceIcmp("TX-ECRIT-COM", next.Ip, next.Ip.Length);
                            }
                        }
                        finally { Interlocked.Exchange(ref writingSince, 0); }
                    }
                }
            }
            catch (OperationCanceledException ex) { if (!StopRequested) Fail(ex); }
            catch (Exception ex) { Fail(ex); }
        }
        public void SetFilter(bool quietIpv4, bool onlyPing)
        {
            quiet = quietIpv4;
            pingOnly = onlyPing;
            pendingPackets.Clear(); // Only whole packets not yet handed to COM.
            Note("MODE IPv4-discret=" + quiet + " ICMP-seul=" + pingOnly +
                 "; file logicielle videe, trame COM en cours preservee.");
        }
        private void Note(string text)
        {
            lock (traceLock)
            {
                if (traces.Count == 100) traces.Dequeue();
                traces.Enqueue(DateTime.Now.ToString("HH:mm:ss.fff") + " " + text);
            }
        }
        private void TraceIcmp(string stage, byte[] packet, int count)
        {
            string text = PacketPolicy.Icmp(packet, count);
            if (text != null) Note(stage + " ICMP " + text);
        }
        private void CountEcho(byte[] packet, int count, ref long requests, ref long replies)
        {
            int type = PacketPolicy.EchoType(packet, count);
            if (type == 8) Interlocked.Increment(ref requests);
            if (type == 0) Interlocked.Increment(ref replies);
        }
        private string Filter(byte[] packet, int count)
        {
            return PacketPolicy.Reject(packet, count, quiet, pingOnly, broadcasts);
        }
        private void ReadWintun()
        {
            try
            {
                IntPtr stopHandle = stop.SafeWaitHandle.DangerousGetHandle();
                while (!StopRequested)
                {
                    uint size;
                    IntPtr source = api.Receive(session, out size);
                    if (source == IntPtr.Zero)
                    {
                        int code = Marshal.GetLastWin32Error();
                        if (code != Native.ErrorNoMoreItems) throw Native.Error("WintunReceivePacket", code);
                        uint wait = Native.WaitForMultipleObjects(2, new IntPtr[] { stopHandle, readEvent }, false, Native.Infinite);
                        if (wait == 0) return;
                        if (wait != 1) throw Native.Error("Wait for Wintun packet");
                        continue;
                    }
                    byte[] packet = null;
                    try
                    {
                        if (size <= (uint)mtu && size >= 20)
                        { packet = new byte[(int)size]; Marshal.Copy(source, packet, 0, (int)size); }
                    }
                    finally { api.Release(session, source); }
                    Interlocked.Increment(ref seenPackets);
                    if (packet == null || !Framing.IsIpPacket(packet, packet.Length, mtu))
                    { Interlocked.Increment(ref txDropped); continue; }
                    CountEcho(packet, packet.Length, ref seenEchoRequest, ref seenEchoReply);
                    TraceIcmp("TX-VU-WINTUN", packet, packet.Length);
                    if ((packet[0] >> 4) == 6) Interlocked.Increment(ref seenIpv6);
                    else if (packet[9] == 1) Interlocked.Increment(ref seenIcmp);
                    else if (packet[9] == 6) Interlocked.Increment(ref seenTcp);
                    else if (packet[9] == 17) Interlocked.Increment(ref seenUdp);
                    string reason = Filter(packet, packet.Length);
                    if (reason != null) { Interlocked.Increment(ref txFiltered); continue; }
                    QueuedPacket next = new QueuedPacket();
                    next.Ip = packet;
                    next.Wire = Framing.Encode(packet, packet.Length);
                    next.High = PacketPolicy.Priority(packet, packet.Length);
                    next.Created = Stopwatch.GetTimestamp();
                    if (pendingPackets.Put(next, next.Created)) queueReady.Set();
                    else TraceIcmp("TX-REJET-FILE", packet, packet.Length);
                }
            }
            catch (OperationCanceledException ex) { if (!StopRequested) Fail(ex); }
            catch (Exception ex) { Fail(ex); }
        }
        private void RefreshAddresses()
        {
            long now = Stopwatch.GetTimestamp();
            if (lastAddressPoll != 0 && now - lastAddressPoll < 3 * Stopwatch.Frequency) return;
            lastAddressPoll = now;
            try
            {
                List<uint> values = new List<uint>();
                List<string> text = new List<string>();
                foreach (NetworkInterface ni in NetworkInterface.GetAllNetworkInterfaces())
                {
                    IPInterfaceProperties props = ni.GetIPProperties();
                    IPv4InterfaceProperties v4;
                    try { v4 = props.GetIPv4Properties(); }
                    catch (NetworkInformationException) { continue; }
                    if (v4 == null || (uint)v4.Index != InterfaceIndex) continue;
                    foreach (UnicastIPAddressInformation item in props.UnicastAddresses)
                    {
                        if (item.Address.AddressFamily != AddressFamily.InterNetwork) continue;
                        byte[] ip = item.Address.GetAddressBytes();
                        byte[] mask = item.IPv4Mask.GetAddressBytes();
                        uint address = PacketPolicy.Address(ip, 0), m = PacketPolicy.Address(mask, 0);
                        text.Add(item.Address.ToString() + " masque " + item.IPv4Mask.ToString());
                        // /31 and /32 do not use directed subnet broadcast.
                        if (m != UInt32.MaxValue && m != 0xFFFFFFFEu && m != 0)
                            values.Add((address & m) | ~m);
                    }
                }
                broadcasts = values.ToArray(); // Immutable snapshot; atomic reference replacement.
                addressText = text.Count == 0 ? "aucune IPv4 sur SerialIP" : String.Join(" ; ", text.ToArray());
            }
            catch (Exception ex) { addressText = "lecture IP impossible : " + ex.Message; }
        }
        private void PollSerial()
        {
            if (serial == null || disposed) return;
            Native.ComStat status;
            uint errors;
            if (Native.ClearCommError(serial.Handle, out errors, out status))
            {
                lastComFlags = status.Flags;
                comIn = status.InQueue; comOut = status.OutQueue;
                cumulativeErrors |= errors;
                bool held = (status.Flags & 8) != 0;
                if (held && !xoffHeld) { xoffStart = Stopwatch.GetTimestamp(); Note("COM : XOFF recu, emission suspendue."); }
                if (!held && xoffHeld) Note("COM : pause XOFF terminee.");
                xoffHeld = held;
                if (errors != 0) Note("COM : erreurs masque 0x" + errors.ToString("X"));
                comStatusOk = true;
            }
            else { comStatusOk = false; comStatusError = Marshal.GetLastWin32Error(); }
        }
        public string DiagnosticReport()
        {
            string snapshot = Statistics();
            StringBuilder text = new StringBuilder();
            text.AppendLine("SerialWintun " + BuildId);
            text.AppendLine(DateTime.Now.ToString("O"));
            text.AppendLine("Port " + port + " ; 115200/8N1/XON-XOFF ; MTU " + mtu);
            text.AppendLine("InterfaceIndex=" + InterfaceIndex + "; IPv4 discret=" + quiet + "; ICMP seul=" + pingOnly);
            text.AppendLine(snapshot);
            text.AppendLine("\r\nDernieres traces (metadonnees ICMP, pas de contenu applicatif) :");
            lock (traceLock) foreach (string line in traces) text.AppendLine(line);
            text.AppendLine("\r\nTX COM = octets acceptes par WriteFile, PAS confirmation de livraison distante.");
            text.AppendLine("COMSTAT ne mesure pas les tampons internes des modems. CRC valide ne prouve pas une reponse au ping.");
            return text.ToString();
        }
        public void RequestStop() { if (!disposed) stop.Set(); }
        public string Statistics()
        {
            RefreshAddresses();
            PollSerial();
            long now = Stopwatch.GetTimestamp();
            long tx = Interlocked.Read(ref txWire), rx = Interlocked.Read(ref rxWire);
            if (lastRateTick == 0) { lastRateTick = now; previousTxWire = tx; previousRxWire = rx; }
            double seconds = (now - lastRateTick) / (double)Stopwatch.Frequency;
            if (seconds >= 0.5)
            {
                txRate = (tx - previousTxWire) / seconds;
                rxRate = (rx - previousRxWire) / seconds;
                lastRateTick = now; previousTxWire = tx; previousRxWire = rx;
            }
            long writing = Interlocked.Read(ref writingSince);
            double writeSeconds = writing == 0 ? 0 : (now - writing) / (double)Stopwatch.Frequency;
            string flow = comStatusOk ? String.Format(
                "COM : XOFF recu={0} ({1:F1}s) ; XOFF emis={2} ; RX {3} o / TX {4} o en attente ; erreurs=0x{5:X}",
                xoffHeld ? "OUI" : "non", xoffHeld ? (now-xoffStart)/(double)Stopwatch.Frequency : 0,
                (lastComFlags & 16) != 0 ? "OUI" : "non", comIn, comOut, cumulativeErrors)
                : "COMSTAT indisponible (pilote), erreur " + comStatusError;
            return String.Format(
                "IPv4 : {0}\r\n" +
                "IP : TX {1} p / {2} o ; RX injectes {3} p / {4} o ; rejets invalides TX {5}, RX {6}, CRC {7}\r\n" +
                "COM application : TX {8:F0} o/s, RX {9:F0} o/s ; total {10}/{11} o ; ecriture en cours {12:F1} s\r\n" +
                "{13}\r\n{14}\r\n" +
                "Filtres TX/RX : {15}/{16} ; vus Wintun IPv6 {17}, ICMP {18}, TCP {19}, UDP {20}\r\n" +
                "Echo REQ/REP : vus TX {21}/{22} ; ecrits COM {23}/{24} ; recus COM {25}/{26} ; injectes {27}/{28}",
                addressText, Interlocked.Read(ref txPackets), Interlocked.Read(ref txBytes),
                Interlocked.Read(ref rxPackets), Interlocked.Read(ref rxBytes),
                Interlocked.Read(ref txDropped), Interlocked.Read(ref rxDropped), decoder == null ? 0 : decoder.BadFrames,
                txRate, rxRate, tx, rx, writeSeconds, flow, pendingPackets.Snapshot(now),
                Interlocked.Read(ref txFiltered), Interlocked.Read(ref rxFiltered),
                Interlocked.Read(ref seenIpv6), Interlocked.Read(ref seenIcmp), Interlocked.Read(ref seenTcp), Interlocked.Read(ref seenUdp),
                Interlocked.Read(ref seenEchoRequest), Interlocked.Read(ref seenEchoReply),
                Interlocked.Read(ref txEchoRequest), Interlocked.Read(ref txEchoReply),
                Interlocked.Read(ref rxEchoRequest), Interlocked.Read(ref rxEchoReply),
                Interlocked.Read(ref injectEchoRequest), Interlocked.Read(ref injectEchoReply));
        }
        private static bool JoinWorker(Thread worker)
        {
            if (worker == null || (worker.ThreadState & System.Threading.ThreadState.Unstarted) != 0) return true;
            return worker.Join(5000);
        }
        // Called by the WinForms timer, on the SAME thread that called Start.
        // Never release the adapter or I/O buffers while a worker can still use them.
        public bool TryCompleteStop()
        {
            if (disposed) return true;
            stop.Set();
            if ((serialReader != null && serialReader.IsAlive) ||
                (serialWriter != null && serialWriter.IsAlive) ||
                (wintunReader != null && wintunReader.IsAlive)) return false;
            if (serial != null) { serial.Dispose(); serial = null; }
            if (session != IntPtr.Zero) { api.End(session); session = IntPtr.Zero; }
            if (adapter != IntPtr.Zero) { api.Close(adapter); adapter = IntPtr.Zero; }
            if (api != null) { api.Dispose(); api = null; }
            if (ownerMutex != null)
            {
                if (ownMutex) { ownerMutex.ReleaseMutex(); ownMutex = false; }
                ownerMutex.Dispose();
                ownerMutex = null;
            }
            pendingPackets.Clear();
            queueReady.Dispose();
            stop.Dispose();
            disposed = true;
            return true;
        }

        public void Dispose()
        {
            if (disposed) return;
            stop.Set();
            bool readerDone = JoinWorker(serialReader);
            bool writerDone = JoinWorker(serialWriter);
            bool pumpDone = JoinWorker(wintunReader);
            if (!readerDone || !writerDone || !pumpDone || !TryCompleteStop())
            {
                // A faulty driver may refuse to finish an outstanding I/O request.
                throw new IOException("Le pilote serie ne termine pas les entrees/sorties. " +
                    "Nettoyage incomplet : les ressources ne sont pas liberees tant que le pilote les utilise. " +
                    "Une fermeture forcee ne garantit pas la suppression de la carte.");
            }
        }

        public static string SelfTest()
        {
            if (Marshal.SizeOf(typeof(Native.Dcb)) != 28) throw new Exception("DCB layout mismatch.");
            if (Marshal.SizeOf(typeof(Native.Overlapped)) != (IntPtr.Size == 8 ? 32 : 20))
                throw new Exception("OVERLAPPED layout mismatch.");
            byte[] check = Encoding.ASCII.GetBytes("123456789");
            ushort fcs = 0xFFFF;
            foreach (byte value in check) fcs = Framing.UpdateFcs(fcs, value);
            if ((ushort)(fcs ^ 0xFFFF) != 0x906E) throw new Exception("CRC check vector failed.");
            Random random = new Random(4261);
            int cases = 0;
            for (int test = 0; test < 300; test++)
            {
                int length = test == 0 ? 256 : test == 1 ? 0 : test == 2 ? 1500 : random.Next(0, 1501);
                byte[] original = new byte[length];
                if (test == 0) for (int i = 0; i < 256; i++) original[i] = (byte)i;
                else random.NextBytes(original);
                byte[] encoded = Framing.Encode(original, original.Length);
                int delivered = 0;
                Decoder decoder = new Decoder(1500, delegate(byte[] bytes, int count)
                {
                    if (count != original.Length) throw new Exception("Round-trip length mismatch.");
                    for (int i = 0; i < count; i++) if (bytes[i] != original[i]) throw new Exception("Round-trip mismatch.");
                    delivered++;
                });
                for (int i = 0; i < encoded.Length; i++)
                {
                    if (i != 0 && i != encoded.Length - 1 && encoded[i] < 0x20)
                        throw new Exception("Unescaped control character on the wire.");
                    decoder.PushByte(encoded[i]);
                    decoder.PushByte(0x11); // Even between 0x7D and the escaped octet.
                    decoder.PushByte(0x13);
                }
                if (delivered != 1 || decoder.BadFrames != 0) throw new Exception("Round-trip delivery failure.");
                cases++;
            }
            int valid = 0;
            Decoder recovery = new Decoder(64, delegate(byte[] bytes, int count) { valid++; });
            byte[] damaged = Framing.Encode(new byte[] { 0x45, 0x40, 0x40 }, 3);
            damaged[1] ^= 1;
            recovery.Push(damaged, damaged.Length);
            byte[] oversized = Framing.Encode(new byte[128], 128);
            recovery.Push(oversized, oversized.Length);
            recovery.Push(new byte[] { 0x7E, 0x7D, 0x7E }, 3);
            byte[] good = Framing.Encode(check, check.Length);
            recovery.Push(good, good.Length);
            if (valid != 1 || recovery.BadFrames != 3) throw new Exception("CRC/recovery/overflow tests failed.");
            byte[] ipv4 = new byte[20]; ipv4[0] = 0x45; ipv4[3] = 20;
            byte[] ipv6 = new byte[40]; ipv6[0] = 0x60;
            if (!Framing.IsIpPacket(ipv4, 20, 1500) || !Framing.IsIpPacket(ipv6, 40, 1500))
                throw new Exception("IP validation failed.");
            ipv4[3] = 21;
            if (Framing.IsIpPacket(ipv4, 20, 1500)) throw new Exception("Invalid IP length accepted.");
            if (Marshal.SizeOf(typeof(Native.ComStat)) != 12) throw new Exception("COMSTAT ABI mismatch.");
            byte[] p = new byte[60]; p[0] = 0x45; p[3] = 60; p[9] = 1;
            p[16] = 10; p[17] = 200; p[18] = 200; p[19] = 1; p[20] = 8;
            if (PacketPolicy.Reject(p, p.Length, true, true, null) != null ||
                !PacketPolicy.Priority(p, p.Length) || PacketPolicy.EchoType(p, p.Length) != 8)
                throw new Exception("ICMP classification failed.");
            p[9] = 6;
            if (PacketPolicy.Reject(p, p.Length, true, false, null) != null ||
                PacketPolicy.Reject(p, p.Length, true, true, null) == null)
                throw new Exception("TCP policy failed.");
            p[16] = 224;
            if (PacketPolicy.Reject(p, p.Length, true, false, null) == null)
                throw new Exception("Multicast policy failed.");
            p[16] = 10; p[19] = 255;
            if (PacketPolicy.Reject(p, p.Length, true, false, new uint[] { 0x0AC8C8FF }) == null)
                throw new Exception("Directed broadcast policy failed.");
            if (PacketPolicy.Reject(ipv6, 40, true, false, null) == null ||
                PacketPolicy.Reject(ipv6, 40, false, false, null) != null)
                throw new Exception("IPv6 policy failed.");
            PacketQueue q = new PacketQueue(10, 100);
            QueuedPacket a = new QueuedPacket(); a.Wire = new byte[6]; a.Created = 0;
            QueuedPacket b = new QueuedPacket(); b.Wire = new byte[6]; b.Created = 1; b.High = true;
            if (!q.Put(a, 0) || !q.Put(b, 1) || q.Bytes != 6 || q.Dropped != 1 || q.Take(1) != b)
                throw new Exception("Bounded queue priority failed.");
            if (!q.Put(a, 1) || q.Take(101) != null || q.Expired != 1 || q.Bytes != 0)
                throw new Exception("Queue expiry failed.");
            return "SelfTest OK: filter/priority/queue-expiry, COMSTAT, ABI layouts, CRC-16, " + cases + " framing round-trips, control escaping, " +
                   "corruption/oversize recovery and IPv4/IPv6 lengths. No Windows driver or modem test performed.";
        }
    }
}
'@

$uiReady = $false
$bridge = $null
try {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        throw 'Ce script graphique nécessite Windows avec un bureau interactif.'
    }
    if ($ExecutionContext.SessionState.LanguageMode -ne 'FullLanguage') {
        throw 'Le mode de sécurité de cette session interdit ce programme. Consulte l''administrateur ; ne contourne pas la politique de l''ordinateur.'
    }
    Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
    Add-Type -AssemblyName System.Drawing -ErrorAction Stop
    $uiReady = $true
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    try {
        $principal = [Security.Principal.WindowsPrincipal]::new($identity)
        $isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    }
    finally { $identity.Dispose() }

    if (-not $GuiChild) {
        # Processus dédié, fenêtré et STA. Aucun COM/carte n'est ouvert dans le parent.
        $psExe = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
        if (-not [Environment]::Is64BitProcess -and [Environment]::Is64BitOperatingSystem) {
            $psExe = Join-Path $env:WINDIR 'Sysnative\WindowsPowerShell\v1.0\powershell.exe'
        }
        $quotedScript = $PSCommandPath.Replace("'", "''")
        $launchCode = "try { & '$quotedScript' -GuiChild } catch { Add-Type -AssemblyName System.Windows.Forms; [void][System.Windows.Forms.MessageBox]::Show(`$_.Exception.Message, 'SerialWintun : lancement impossible') }"
        $arguments = '-NoLogo -NoProfile -STA -WindowStyle Hidden -ExecutionPolicy RemoteSigned -Command "' + $launchCode + '"'
        $options = @{
            FilePath = $psExe
            ArgumentList = $arguments
            WindowStyle = 'Hidden'
            WorkingDirectory = $PSScriptRoot
            ErrorAction = 'Stop'
        }
        if (-not $isAdmin) { $options['Verb'] = 'RunAs' }
        try { Start-Process @options }
        catch {
            Show-AppMessage -Text ("Démarrage annulé ou impossible. Aucune carte créée.`r`n" + $_.Exception.Message) -Icon Warning
        }
        return
    }
    if (-not $isAdmin) { throw 'L''élévation administrateur est nécessaire pour créer la carte Wintun.' }
    if ($PSVersionTable.PSEdition -ne 'Desktop') {
        throw 'Le lanceur doit utiliser Windows PowerShell 5.1, et non PowerShell 7.'
    }
    if ([Threading.Thread]::CurrentThread.GetApartmentState() -ne [Threading.ApartmentState]::STA) {
        throw 'Le lancement graphique doit se faire en mode STA. Utilise le lanceur fourni ou le menu Exécuter avec PowerShell.'
    }
    [System.Windows.Forms.Application]::EnableVisualStyles()
    [System.Windows.Forms.Application]::SetUnhandledExceptionMode(
        [System.Windows.Forms.UnhandledExceptionMode]::ThrowException)

    # Toujours demander le fichier ; pas de recherche automatique ni de chemin à saisir.
    $dllPath = Select-WintunLibrary
    if ([string]::IsNullOrWhiteSpace($dllPath)) { return }
    $port = Select-SerialPort -DllPath $dllPath
    if ([string]::IsNullOrWhiteSpace($port)) { return }

    Import-Module NetAdapter -ErrorAction Stop
    Import-Module NetTCPIP -ErrorAction Stop
    $existing = @(Get-NetAdapter -IncludeHidden -ErrorAction Stop |
        Where-Object { $_.Name -eq $script:AdapterName })
    if ($existing.Count -gt 0) {
        throw "Une carte '$($script:AdapterName)' existe déjà. Elle ne sera ni réutilisée ni supprimée.`r`nFerme d'abord l'instance qui la possède, ou renomme cette carte dans les connexions réseau."
    }
    # Revalider le fichier juste avant son chargement.
    $dllPath = Test-WintunLibrary -Path $dllPath
    if ($null -eq ('SerialWintunV1.Bridge' -as [type])) {
        Add-Type -TypeDefinition $source -Language CSharp -ErrorAction Stop
    }
    elseif ([SerialWintunV1.Bridge]::BuildId -ne '2026-10-31-gui-r4') {
        throw 'Une autre version du moteur est déjà chargée dans ce processus. Relance le script depuis l''Explorateur.'
    }
    # Autotests du codec, silencieux, avant la création de la carte.
    $null = [SerialWintunV1.Bridge]::SelfTest()
    $bridge = [SerialWintunV1.Bridge]::new($dllPath, $port,
        $script:AdapterName, $script:Mtu, $true, $true)
    Show-ActiveBridge -Bridge $bridge -Port $port -DllPath $dllPath
}
catch {
    $failureText = $_.Exception.Message
    if ($null -ne $bridge -and -not $bridge.IsDisposed) {
        try { $bridge.Dispose() }
        catch { $failureText += "`r`n`r`nNettoyage incomplet : " + $_.Exception.Message }
    }
    if ($uiReady) { Show-AppMessage -Text $failureText -Title 'Liaison série / Wintun — erreur' -Icon Error }
    else { throw }
}
finally {
    # Dernier filet de sécurité pour une fermeture inattendue de la fenêtre.
    if ($null -ne $bridge -and -not $bridge.IsDisposed) {
        try { $bridge.Dispose() }
        catch {
            if ($uiReady) {
                Show-AppMessage -Text $_.Exception.Message -Title 'Nettoyage non terminé' -Icon Warning
            }
        }
    }
}
