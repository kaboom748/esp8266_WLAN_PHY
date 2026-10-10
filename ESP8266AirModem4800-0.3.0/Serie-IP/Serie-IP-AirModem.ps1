#requires -Version 5.1
<#
.SYNOPSIS
Passerelle IPv4 bidirectionnelle Wintun <-> serie, 4800/8N1/XON-XOFF.
.DESCRIPTION
Un seul PS1 avec un moteur C# integre. Charge uniquement wintun.dll situee
au meme endroit que ce script. Cree son propre adaptateur temporaire.
Cible : Windows PowerShell 5.1 Desktop x64 sur Windows x64 Intel/AMD.
Ne pas executer avec pwsh (PowerShell 7) pour cette version.
Cadrage V1 conserve : decodeur compatible avec Serie-IP.ps1.
Mode Minimal par defaut pour ESP8266AirModem4800 0.3.0 en 8N1.
Mode Conservative : echappement historique, pour comparaison ou chemin non transparent.
Ne change ni le firmware, ni les temporisations radio, ni les paquets IP.
Aucun chiffrement/authentification, aucune retransmission de couche liaison.
Ne modifie pas la route par defaut. Ne reutilise aucun adaptateur existant.
.EXAMPLE
.\Serie-IP-AirModem.ps1 -Role A -Port COM3 -AllowPing
.EXAMPLE
.\Serie-IP-AirModem.ps1 -Role B -Port COM4 -AllowPing
.EXAMPLE
.\Serie-IP-AirModem.ps1 -SelfTest
.EXAMPLE
.\Serie-IP-AirModem.ps1 -ListPorts
.NOTES
Prototype fourni en source, a valider sur les deux PC. DLL non fournie.
Version AirModem 2.0 : optimisation de l'echappement uniquement, statistiques ajoutees.
Sources API, limites et tests expliques dans LIRE-MOI.md.
#>
[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Run')]
    [ValidateSet('A','B')]
    [string]$Role,

    [Parameter(Mandatory = $true, ParameterSetName = 'Run')]
    [ValidatePattern('^COM[1-9][0-9]*$')]
    [string]$Port,

    [Parameter(ParameterSetName = 'Run')]
    [ValidatePattern('^[A-Za-z][A-Za-z0-9_-]{0,40}$')]
    [string]$AdapterName = 'SerieIP',

    [Parameter(ParameterSetName = 'Run')]
    [string]$LocalIp,
    [Parameter(ParameterSetName = 'Run')]
    [string]$PeerIp,

    [Parameter(ParameterSetName = 'Run')]
    [ValidateRange(1200,115200)]
    [int]$BaudRate = 4800,
    [Parameter(ParameterSetName = 'Run')]
    [ValidateSet('Minimal','Conservative')]
    [string]$EscapeMode = 'Minimal',
    [Parameter(ParameterSetName = 'Run')]
    [ValidateRange(576,1500)]
    [int]$Mtu = 576,
    [Parameter(ParameterSetName = 'Run')]
    [ValidateRange(1,64)]
    [int]$QueuePackets = 4,
    [Parameter(ParameterSetName = 'Run')]
    [ValidateRange(5,120)]
    [int]$WriteTimeoutSeconds = 30,
    [Parameter(ParameterSetName = 'Run')]
    [ValidateRange(0,86400)]
    [int]$DurationSeconds = 0,

    [Parameter(ParameterSetName = 'Run')]
    [switch]$NoDtr,
    [Parameter(ParameterSetName = 'Run')]
    [switch]$NoRts,
    [Parameter(ParameterSetName = 'Run')]
    [switch]$AllowPing,

    [Parameter(Mandatory = $true, ParameterSetName = 'SelfTest')]
    [switch]$SelfTest,
    [Parameter(Mandatory = $true, ParameterSetName = 'ListPorts')]
    [switch]$ListPorts
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'Ce script necessite Windows.'
}
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5) {
    throw 'Utiliser Windows PowerShell 5.1 (powershell.exe), pas PowerShell 7/pwsh pour cette version.'
}
if ($ListPorts) {
    $ports = @([IO.Ports.SerialPort]::GetPortNames() | Sort-Object)
    if ($ports.Count -eq 0) { Write-Warning 'Aucun port COM detecte.' }
    else { $ports }
    return
}

# The C# engine has no PowerShell scriptblocks on worker threads.
# Compile once per PowerShell process; restart the process after editing it.
$source = @'
using System;
using System.IO;
using System.IO.Ports;
using System.Net;
using System.Text;
using System.Collections.Generic;
using System.Collections.Concurrent;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

namespace SerieIpAirModemV2
{
    // Original framing protocol: NOT PPP and NOT SLIP.
    // 7E | escaped(version=1, type=1, length BE16, IPv4, CRC BE16) | 7E
    // CRC-16/CCITT-FALSE: poly=1021, init=FFFF, refin=false, xorout=0.
    public static class Framing
    {
        public static ushort Crc(byte[] bytes, int count)
        {
            ushort crc = 0xffff;
            for (int i = 0; i < count; i++)
            {
                crc ^= (ushort)(bytes[i] << 8);
                for (int j = 0; j < 8; j++)
                    crc = (ushort)((crc & 0x8000) != 0 ? (crc << 1) ^ 0x1021 : crc << 1);
            }
            return crc;
        }

        private static bool MustEscape(byte value, bool minimalEscape)
        {
            // AirModem 0.3.0 intercepts only literal 11/13. The PC decoder uses
            // 7D/7E. All other octets, including NUL and 91/93, stay binary.
            // Minimal requires a transparent 8-bit path: never use text I/O.
            if (minimalEscape)
                return value == 0x11 || value == 0x13 || value == 0x7d || value == 0x7e;
            // Historical V1 policy, retained unchanged for A/B testing.
            return value < 0x20 || value == 0x7d || value == 0x7e ||
                   value == 0x91 || value == 0x93;
        }

        private static byte[] Escape(byte[] body, bool minimalEscape)
        {
            List<byte> wire = new List<byte>(body.Length * 2 + 2);
            wire.Add(0x7e);
            foreach (byte value in body)
            {
                if (MustEscape(value, minimalEscape)) { wire.Add(0x7d); wire.Add((byte)(value ^ 0x20)); }
                else wire.Add(value);
            }
            wire.Add(0x7e);
            return wire.ToArray();
        }

        // The one-argument overload retains the original conservative encoding.
        public static byte[] Encode(byte[] packet) { return Encode(packet, false); }

        public static byte[] Encode(byte[] packet, bool minimalEscape)
        {
            if (packet == null || packet.Length < 1 || packet.Length > 65535)
                throw new ArgumentException("Invalid frame payload length.");
            byte[] body = new byte[packet.Length + 6];
            body[0] = 1; body[1] = 1;
            body[2] = (byte)(packet.Length >> 8); body[3] = (byte)packet.Length;
            Buffer.BlockCopy(packet, 0, body, 4, packet.Length);
            ushort crc = Crc(body, body.Length - 2);
            body[body.Length - 2] = (byte)(crc >> 8);
            body[body.Length - 1] = (byte)crc;
            return Escape(body, minimalEscape);
        }

        public sealed class Decoder
        {
            private readonly byte[] body;
            private readonly Action<byte[]> receiver;
            private bool started, escaped, overflow;
            private int count;
            private long errors, good;
            public long Errors { get { return Interlocked.Read(ref errors); } }
            public long Good { get { return Interlocked.Read(ref good); } }

            public Decoder(int maxPacketSize, Action<byte[]> receiver)
            {
                if (maxPacketSize < 1 || maxPacketSize > 65535 || receiver == null)
                    throw new ArgumentException("Invalid decoder configuration.");
                this.body = new byte[maxPacketSize + 6];
                this.receiver = receiver;
            }

            private void Complete()
            {
                if (escaped || count < 7 || body[0] != 1 || body[1] != 1)
                { Interlocked.Increment(ref errors); return; }
                int size = (body[2] << 8) | body[3];
                if (size < 1 || size + 6 != count)
                { Interlocked.Increment(ref errors); return; }
                ushort expected = (ushort)((body[count - 2] << 8) | body[count - 1]);
                if (Crc(body, count - 2) != expected)
                { Interlocked.Increment(ref errors); return; }
                byte[] packet = new byte[size];
                Buffer.BlockCopy(body, 4, packet, 0, size);
                Interlocked.Increment(ref good);
                receiver(packet);
            }

            public void Feed(byte[] bytes, int offset, int length)
            {
                if (bytes == null || offset < 0 || length < 0 || offset > bytes.Length - length)
                    throw new ArgumentOutOfRangeException("length");
                for (int i = offset; i < offset + length; i++)
                {
                    byte value = bytes[i];
                    // Normally consumed by the Windows serial driver. Ignoring
                    // unescaped XON/XOFF here also tolerates controls in test streams.
                    if (value == 0x11 || value == 0x13) continue;
                    if (value == 0x7e)
                    {
                        if (started && !overflow && (count > 0 || escaped)) Complete();
                        started = true; escaped = false; overflow = false; count = 0;
                        continue;
                    }
                    if (!started || overflow) continue;
                    if (escaped) { value ^= 0x20; escaped = false; }
                    else if (value == 0x7d) { escaped = true; continue; }
                    if (count == body.Length)
                    { overflow = true; Interlocked.Increment(ref errors); continue; }
                    body[count++] = value;
                }
            }
        }

        private static void Check(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException("Self-test: " + message);
        }

        private static string TestMode(bool minimalEscape)
        {
            byte[] check = Encoding.ASCII.GetBytes("123456789");
            Check(Crc(check, check.Length) == 0x29b1, "CRC reference vector");
            Random rng = new Random(4800);
            int accepted = 0;
            byte[] expected = null;
            Decoder decoder = new Decoder(1500, delegate(byte[] received) {
                Check(expected != null && received.Length == expected.Length, "length");
                for (int i = 0; i < received.Length; i++)
                    Check(received[i] == expected[i], "round trip");
                accepted++;
            });
            for (int test = 0; test < 1000; test++)
            {
                expected = new byte[test == 0 ? 256 : rng.Next(1, 1501)];
                if (test == 0)
                    for (int i = 0; i < 256; i++) expected[i] = (byte)i;
                else rng.NextBytes(expected);
                byte[] frame = Encode(expected, minimalEscape);
                for (int i = 1; i < frame.Length - 1; i++)
                {
                    Check(frame[i] != 0x11 && frame[i] != 0x13 && frame[i] != 0x7e,
                          "unescaped flow control or delimiter");
                    if (!minimalEscape)
                        Check(frame[i] >= 0x20 && frame[i] != 0x91 && frame[i] != 0x93,
                              "conservative encoding");
                }
                if (minimalEscape)
                    Check(frame.Length <= Encode(expected, false).Length, "non-expansion vs conservative");
                // Byte-at-a-time for all values, random chunks for the remainder.
                int position = 0;
                while (position < frame.Length)
                {
                    int length = Math.Min(test == 0 ? 1 : rng.Next(1, 80), frame.Length - position);
                    decoder.Feed(frame, position, length);
                    // Insert actual flow-control bytes even in the middle of an escape.
                    byte[] controls = new byte[] { 0x11, 0x13 };
                    decoder.Feed(controls, 0, controls.Length);
                    position += length;
                }
            }
            Check(accepted == 1000 && decoder.Errors == 0, "random frames");
            // A syntactically correct frame with a deliberately wrong CRC.
            byte[] badBody = new byte[] { 1, 1, 0, 1, 0x45, 0, 0 };
            ushort goodCrc = Crc(badBody, 5);
            badBody[5] = (byte)(goodCrc >> 8); badBody[6] = (byte)(goodCrc ^ 1);
            byte[] badFrame = Escape(badBody, minimalEscape);
            decoder.Feed(badFrame, 0, badFrame.Length);
            Check(accepted == 1000 && decoder.Errors == 1, "CRC rejection");
            // Oversize input is discarded, then decoding resynchronizes.
            byte[] oversize = new byte[1602];
            for (int i = 0; i < oversize.Length; i++) oversize[i] = 0x41;
            oversize[0] = 0x7e; oversize[oversize.Length - 1] = 0x7e;
            decoder.Feed(oversize, 0, oversize.Length);
            expected = new byte[] { 0x45, 0x11, 0x13, 0x7d, 0x7e };
            byte[] last = Encode(expected, minimalEscape);
            decoder.Feed(last, 0, last.Length);
            Check(accepted == 1001 && decoder.Errors == 2, "overflow recovery");
            // An unterminated escape must not poison the next frame.
            byte[] dangling = new byte[] { 0x7e, 0x7d, 0x7e };
            decoder.Feed(dangling, 0, dangling.Length);
            decoder.Feed(last, 0, last.Length);
            Check(accepted == 1002 && decoder.Errors == 3, "escape recovery");
            return (minimalEscape ? "Minimal" : "Conservative") +
                " OK: CRC, 256 valeurs, 1000 trames, lectures fragmentees, XON/XOFF, erreurs et resynchronisation.";
        }

        private static ushort InternetChecksum(byte[] bytes, int offset, int count)
        {
            uint sum = 0;
            while (count > 1)
            {
                sum += (uint)((bytes[offset] << 8) | bytes[offset + 1]);
                offset += 2; count -= 2;
            }
            if (count != 0) sum += (uint)(bytes[offset] << 8);
            while ((sum >> 16) != 0) sum = (sum & 0xffff) + (sum >> 16);
            return (ushort)(~sum);
        }

        // Synthetic example only: not a measurement of the user's packets or RTT.
        public static byte[] ExampleEcho(bool reply)
        {
            byte[] p = new byte[29];
            p[0] = 0x45; p[3] = 29; p[4] = 0x12; p[5] = 0x34;
            p[8] = 128; p[9] = 1;
            p[12] = 10; p[13] = 77; p[15] = (byte)(reply ? 1 : 2);
            p[16] = 10; p[17] = 77; p[19] = (byte)(reply ? 2 : 1);
            p[20] = (byte)(reply ? 0 : 8);
            p[24] = 0x12; p[25] = 0x34; p[27] = 1; p[28] = 0x61;
            ushort icmp = InternetChecksum(p, 20, 9);
            p[22] = (byte)(icmp >> 8); p[23] = (byte)icmp;
            ushort ip = InternetChecksum(p, 0, 20);
            p[10] = (byte)(ip >> 8); p[11] = (byte)ip;
            return p;
        }

        public static string SelfTest()
        {
            string result = TestMode(false) + Environment.NewLine + TestMode(true);
            foreach (bool reply in new bool[] { false, true })
            {
                byte[] p = ExampleEcho(reply);
                Check(InternetChecksum(p, 0, 20) == 0 && InternetChecksum(p, 20, 9) == 0,
                      "synthetic IP/ICMP checksums");
                result += Environment.NewLine + String.Format(
                    "Exemple synthetique {0}: IP {1} o; Conservative {2} o; Minimal {3} o (pas un RTT mesure).",
                    reply ? "reponse" : "requete", p.Length, Encode(p, false).Length, Encode(p, true).Length);
            }
            return result;
        }
    }

    internal sealed class WintunApi : IDisposable
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
        private static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true, ExactSpelling = true)]
        private static extern IntPtr GetProcAddress(IntPtr module, string name);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool FreeLibrary(IntPtr module);
        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
        [DllImport("iphlpapi.dll", ExactSpelling = true)]
        internal static extern uint ConvertInterfaceLuidToIndex(ref ulong luid, out uint index);

        [UnmanagedFunctionPointer(CallingConvention.Winapi, CharSet = CharSet.Unicode, SetLastError = true)]
        internal delegate IntPtr CreateFn(string name, string tunnelType, ref Guid guid);
        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        internal delegate void CloseFn(IntPtr adapter);
        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        internal delegate void LuidFn(IntPtr adapter, out ulong luid);
        [UnmanagedFunctionPointer(CallingConvention.Winapi, SetLastError = true)]
        internal delegate IntPtr StartFn(IntPtr adapter, uint capacity);
        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        internal delegate void EndFn(IntPtr session);
        [UnmanagedFunctionPointer(CallingConvention.Winapi, SetLastError = true)]
        internal delegate IntPtr EventFn(IntPtr session);
        [UnmanagedFunctionPointer(CallingConvention.Winapi, SetLastError = true)]
        internal delegate IntPtr ReceiveFn(IntPtr session, out uint size);
        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        internal delegate void ReleaseFn(IntPtr session, IntPtr packet);
        [UnmanagedFunctionPointer(CallingConvention.Winapi, SetLastError = true)]
        internal delegate IntPtr AllocateFn(IntPtr session, uint size);
        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        internal delegate void SendFn(IntPtr session, IntPtr packet);

        private IntPtr module;
        internal CreateFn Create;
        internal CloseFn Close;
        internal LuidFn GetLuid;
        internal StartFn Start;
        internal EndFn End;
        internal EventFn GetEvent;
        internal ReceiveFn Receive;
        internal ReleaseFn Release;
        internal AllocateFn Allocate;
        internal SendFn Send;

        internal WintunApi(string path)
        {
            // Absolute path to the DLL beside the PS1, not the current directory.
            // Dependencies restricted to that directory and Windows System32.
            module = LoadLibraryExW(Path.GetFullPath(path), IntPtr.Zero, 0x100 | 0x800);
            if (module == IntPtr.Zero) throw LastError("LoadLibraryExW(wintun.dll)");
            try
            {
                Create = Bind<CreateFn>("WintunCreateAdapter");
                Close = Bind<CloseFn>("WintunCloseAdapter");
                GetLuid = Bind<LuidFn>("WintunGetAdapterLUID");
                Start = Bind<StartFn>("WintunStartSession");
                End = Bind<EndFn>("WintunEndSession");
                GetEvent = Bind<EventFn>("WintunGetReadWaitEvent");
                Receive = Bind<ReceiveFn>("WintunReceivePacket");
                Release = Bind<ReleaseFn>("WintunReleaseReceivePacket");
                Allocate = Bind<AllocateFn>("WintunAllocateSendPacket");
                Send = Bind<SendFn>("WintunSendPacket");
            }
            catch { Dispose(); throw; }
        }

        private T Bind<T>(string name) where T : class
        {
            IntPtr function = GetProcAddress(module, name);
            if (function == IntPtr.Zero) throw LastError("Export absent: " + name);
            return (T)(object)Marshal.GetDelegateForFunctionPointer(function, typeof(T));
        }

        internal static Exception LastError(string operation)
        {
            int code = Marshal.GetLastWin32Error();
            return new Win32Exception(code, operation + ": " + new Win32Exception(code).Message + " (" + code + ")");
        }

        public void Dispose()
        {
            if (module != IntPtr.Zero) { FreeLibrary(module); module = IntPtr.Zero; }
        }
    }

    public sealed class Bridge : IDisposable
    {
        private readonly BlockingCollection<byte[]> outgoing;
        private readonly byte[] local, peer;
        private readonly int mtu, writeTimeoutMs;
        private readonly bool minimalEscape;
        private readonly object frameStatsLock = new object();
        private long shortFrames, longFrames;
        private int lastTxIp, lastTxWire;
        private long lastWriteMs, lastDrainMs;
        private readonly object faultLock = new object();
        private WintunApi api;
        private IntPtr adapter, session, readEvent;
        private SerialPort serial;
        private Thread tunReader, serialReader, serialWriter;
        private Framing.Decoder decoder;
        private volatile bool stopping;
        private bool disposed, started;
        private string failure;
        private long txPackets, rxPackets, txBytes, rxBytes, wireTx, wireRx;
        private long queueDrops, txFiltered, rxFiltered, ringDrops;
        public uint InterfaceIndex { get; private set; }
        public string Failure { get { lock (faultLock) { return failure; } } }

        public Bridge(string dll, string name, Guid guid, string port, int baud, int mtu,
                      int queuePackets, int writeTimeoutMs, bool dtr, bool rts,
                      string localAddress, string peerAddress, bool minimalEscape)
        {
            this.mtu = mtu; this.writeTimeoutMs = writeTimeoutMs;
            this.minimalEscape = minimalEscape;
            local = IPAddress.Parse(localAddress).GetAddressBytes();
            peer = IPAddress.Parse(peerAddress).GetAddressBytes();
            if (local.Length != 4 || peer.Length != 4 || mtu < 576 || mtu > 1500 ||
                queuePackets < 1 || queuePackets > 64 || baud < 1 || writeTimeoutMs < 1000)
                throw new ArgumentException("Invalid bridge parameters.");
            outgoing = new BlockingCollection<byte[]>(queuePackets);
            try
            {
                // Open COM first: a busy/missing port must not leave a new adapter.
                serial = new SerialPort(port, baud, Parity.None, 8, StopBits.One);
                serial.Handshake = Handshake.XOnXOff;
                serial.ReadTimeout = 200;
                serial.WriteTimeout = writeTimeoutMs;
                serial.ReadBufferSize = 4096; serial.WriteBufferSize = 2048;
                serial.DtrEnable = dtr; serial.RtsEnable = rts;
                serial.DiscardNull = false;
                serial.Open();
                api = new WintunApi(dll);
                adapter = api.Create(name, "SerieIP", ref guid);
                if (adapter == IntPtr.Zero) throw WintunApi.LastError("WintunCreateAdapter");
                ulong luid; uint index;
                api.GetLuid(adapter, out luid);
                uint result = WintunApi.ConvertInterfaceLuidToIndex(ref luid, out index);
                if (result != 0) throw new Win32Exception((int)result, "ConvertInterfaceLuidToIndex");
                InterfaceIndex = index;
                // The API requires a power of two, minimum 128 KiB.
                // This is NOT our serial transmit queue; we drain it separately.
                session = api.Start(adapter, 0x20000);
                if (session == IntPtr.Zero) throw WintunApi.LastError("WintunStartSession");
                readEvent = api.GetEvent(session);
                if (readEvent == IntPtr.Zero) throw WintunApi.LastError("WintunGetReadWaitEvent");
                decoder = new Framing.Decoder(mtu, InjectIp);
            }
            catch { Dispose(); throw; }
        }

        public void Start()
        {
            if (disposed || started) throw new InvalidOperationException("Bridge already started or closed.");
            started = true;
            tunReader = MakeThread(ReadWintun, "SerieIP-Wintun");
            serialReader = MakeThread(ReadSerial, "SerieIP-SerialRead");
            serialWriter = MakeThread(WriteSerial, "SerieIP-SerialWrite");
            tunReader.Start(); serialReader.Start(); serialWriter.Start();
        }

        private static Thread MakeThread(ThreadStart action, string name)
        {
            Thread thread = new Thread(action); thread.IsBackground = true; thread.Name = name; return thread;
        }

        private void Fail(string where, Exception error)
        {
            lock (faultLock)
            {
                if (!stopping && failure == null) failure = where + ": " + error.Message;
                stopping = true;
            }
        }

        private bool ValidIp(byte[] packet, bool inbound)
        {
            if (packet.Length < 20 || packet.Length > mtu || (packet[0] >> 4) != 4) return false;
            int header = (packet[0] & 15) * 4;
            if (header < 20 || header > packet.Length || ((packet[2] << 8) | packet[3]) != packet.Length)
                return false;
            byte[] source = inbound ? peer : local;
            byte[] destination = inbound ? local : peer;
            for (int i = 0; i < 4; i++)
                if (packet[12 + i] != source[i] || packet[16 + i] != destination[i]) return false;
            // No L2, multicast, IPv6, or forwarding to third-party hosts.
            // Transport/IP checksums are left to Windows, not rewritten here.
            return true;
        }

        private void ReadWintun()
        {
            try
            {
                while (!stopping)
                {
                    uint size;
                    IntPtr packet = api.Receive(session, out size);
                    if (packet == IntPtr.Zero)
                    {
                        int error = Marshal.GetLastWin32Error();
                        if (error == 259) // ERROR_NO_MORE_ITEMS
                        {
                            uint waited = WintunApi.WaitForSingleObject(readEvent, 100);
                            if (waited == 0xffffffff) throw WintunApi.LastError("WaitForSingleObject");
                            continue;
                        }
                        throw new Win32Exception(error, "WintunReceivePacket: " + new Win32Exception(error).Message);
                    }
                    try
                    {
                        if (size < 20 || size > mtu) { Interlocked.Increment(ref txFiltered); continue; }
                        byte[] bytes = new byte[(int)size];
                        Marshal.Copy(packet, bytes, 0, bytes.Length);
                        if (!ValidIp(bytes, false)) { Interlocked.Increment(ref txFiltered); continue; }
                        if (!outgoing.TryAdd(bytes)) Interlocked.Increment(ref queueDrops);
                    }
                    finally
                    {
                        // Never retain a Wintun pointer across slow serial I/O.
                        api.Release(session, packet);
                    }
                }
            }
            catch (Exception error) { Fail("Lecture Wintun", error); }
        }

        private void WriteSerial()
        {
            try
            {
                while (!stopping)
                {
                    byte[] packet;
                    if (!outgoing.TryTake(out packet, 200)) continue;
                    if (stopping) break;
                    byte[] frame = Framing.Encode(packet, minimalEscape);
                    Stopwatch writing = Stopwatch.StartNew();
                    serial.Write(frame, 0, frame.Length);
                    writing.Stop();
                    // Do not turn a bounded user queue into a large driver backlog.
                    Stopwatch drain = Stopwatch.StartNew();
                    while (!stopping && serial.BytesToWrite > 0)
                    {
                        if (drain.ElapsedMilliseconds >= writeTimeoutMs)
                            throw new TimeoutException("Sortie COM bloquee (XOFF ou pilote serie).");
                        Thread.Sleep(10);
                    }
                    if (stopping) break;
                    Interlocked.Increment(ref txPackets);
                    Interlocked.Add(ref txBytes, packet.Length);
                    Interlocked.Add(ref wireTx, frame.Length);
                    lock (frameStatsLock)
                    {
                        lastTxIp = packet.Length; lastTxWire = frame.Length;
                        lastWriteMs = writing.ElapsedMilliseconds;
                        lastDrainMs = drain.ElapsedMilliseconds;
                        if (frame.Length <= 52) shortFrames++; else longFrames++;
                    }
                }
            }
            catch (TimeoutException error)
            {
                // Write may have sent only part of a frame. Stop, never retry
                // that whole frame blindly and risk duplicating part of the stream.
                Fail("Timeout emission serie; session arretee", error);
            }
            catch (Exception error) { Fail("Ecriture serie", error); }
        }

        private void ReadSerial()
        {
            byte[] bytes = new byte[2048];
            try
            {
                while (!stopping)
                {
                    int length;
                    try { length = serial.Read(bytes, 0, bytes.Length); }
                    catch (TimeoutException) { continue; } // Keep partial frames across idle periods/XOFF.
                    if (length > 0)
                    {
                        Interlocked.Add(ref wireRx, length);
                        decoder.Feed(bytes, 0, length);
                    }
                }
            }
            catch (Exception error) { Fail("Lecture serie", error); }
        }

        private void InjectIp(byte[] packet)
        {
            if (stopping) return;
            if (!ValidIp(packet, true)) { Interlocked.Increment(ref rxFiltered); return; }
            IntPtr buffer = api.Allocate(session, (uint)packet.Length);
            if (buffer == IntPtr.Zero)
            {
                int error = Marshal.GetLastWin32Error();
                if (error == 111) { Interlocked.Increment(ref ringDrops); return; } // ERROR_BUFFER_OVERFLOW
                throw new Win32Exception(error, "WintunAllocateSendPacket");
            }
            Marshal.Copy(packet, 0, buffer, packet.Length);
            api.Send(session, buffer);
            Interlocked.Increment(ref rxPackets);
            Interlocked.Add(ref rxBytes, packet.Length);
        }

        public string Status()
        {
            return String.Format(
                "TX {0} paq/{1} o | RX {2} paq/{3} o | file {4} | pertes file {5}, ring {6} | trames invalides {7} | filtres TX/RX {8}/{9} | serie TX/RX {10}/{11} o",
                Interlocked.Read(ref txPackets), Interlocked.Read(ref txBytes),
                Interlocked.Read(ref rxPackets), Interlocked.Read(ref rxBytes),
                disposed ? 0 : outgoing.Count, Interlocked.Read(ref queueDrops), Interlocked.Read(ref ringDrops),
                decoder == null ? 0 : decoder.Errors, Interlocked.Read(ref txFiltered), Interlocked.Read(ref rxFiltered),
                Interlocked.Read(ref wireTx), Interlocked.Read(ref wireRx));
        }

        // These are PC-side observations, not radio ACKs or radio timing.
        // BytesToWrite == 0 does NOT mean the peer has received the frame.
        public string FrameStatus()
        {
            lock (frameStatsLock)
            {
                if (lastTxWire == 0)
                    return "Cadrage " + (minimalEscape ? "Minimal" : "Conservative") + " | aucune trame TX terminee.";
                return String.Format(
                    "Cadrage {0} | dernier TX: IP {1} o -> COM {2} o | tailles <=52/>52: {3}/{4} | dernier Write/vidange COM: {5}/{6} ms (pas RTT radio)",
                    minimalEscape ? "Minimal" : "Conservative", lastTxIp, lastTxWire,
                    shortFrames, longFrames, lastWriteMs, lastDrainMs);
            }
        }

        private static bool Join(Thread thread, int milliseconds)
        {
            return thread == null || !thread.IsAlive || thread.Join(milliseconds);
        }

        public void Dispose()
        {
            if (disposed) return;
            stopping = true;
            // Purge pending TX to cancel a write held by XOFF before closing COM.
            if (serial != null && serial.IsOpen)
            {
                try { serial.DiscardOutBuffer(); } catch { }
            }
            bool a = Join(tunReader, 2000);
            bool b = Join(serialReader, 2000);
            bool c = Join(serialWriter, writeTimeoutMs + 2000);
            if (!a || !b || !c)
            {
                // Deliberately retain native handles instead of unloading a DLL
                // still executing on a thread. Close the dedicated process.
                throw new InvalidOperationException("Un thread reste bloque. Fermer ce processus PowerShell; ressources natives conservees pour eviter une liberation prematuree.");
            }
            try
            {
                if (serial != null) { serial.Dispose(); serial = null; }
            }
            finally
            {
                if (session != IntPtr.Zero) { api.End(session); session = IntPtr.Zero; }
                // readEvent belongs to the session: NEVER CloseHandle it.
                readEvent = IntPtr.Zero;
                // We created this adapter ourselves; Close removes this adapter.
                // We deliberately do NOT call WintunDeleteDriver.
                if (adapter != IntPtr.Zero) { api.Close(adapter); adapter = IntPtr.Zero; }
                if (api != null) { api.Dispose(); api = null; }
                outgoing.Dispose(); disposed = true;
            }
        }
    }
}
'@
if (-not ('SerieIpAirModemV2.Bridge' -as [type])) {
    Add-Type -TypeDefinition $source -Language CSharp -ReferencedAssemblies 'System.dll','System.Core.dll'
}
if ($SelfTest) {
    [SerieIpAirModemV2.Framing]::SelfTest()
    return
}

# --------------------------- Preflight checks ---------------------------
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
try {
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Ouvrir Windows PowerShell en tant qu administrateur.'
    }
}
finally { $identity.Dispose() }
if (-not [Environment]::Is64BitProcess) {
    throw 'Ouvrir Windows PowerShell x64, pas la version (x86). Utiliser la DLL AMD64.'
}
if ([string]::IsNullOrEmpty($PSScriptRoot)) {
    throw 'Enregistrer ce code dans Serie-IP-AirModem.ps1 puis executer le fichier.'
}
$dll = Join-Path -Path $PSScriptRoot -ChildPath 'wintun.dll'
if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) {
    throw "DLL absente : $dll. Placer la DLL officielle AMD64 dans le dossier du PS1."
}
$dll = (Get-Item -LiteralPath $dll).FullName

# Refuse the wrong machine type before calling native code.
$stream = [IO.File]::OpenRead($dll)
$reader = [IO.BinaryReader]::new($stream)
try {
    if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5A4D) {
        throw 'wintun.dll ne contient pas un executable PE valide.'
    }
    $stream.Position = 0x3C
    $peOffset = $reader.ReadInt32()
    if ($peOffset -lt 64 -or $peOffset -gt ($stream.Length - 6)) {
        throw 'En-tete PE invalide.'
    }
    $stream.Position = $peOffset
    if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x8664) {
        throw 'Cette version du script attend wintun.dll AMD64 (x64), pas x86/ARM.'
    }
}
finally { $reader.Dispose() }
$signature = Get-AuthenticodeSignature -LiteralPath $dll
if ($signature.Status -ne 'Valid') {
    throw "Signature DLL non valide/verifiable ($($signature.Status)). Verifier la copie officielle, l horloge et la confiance des certificats Windows. $($signature.StatusMessage)"
}

function ConvertTo-IPv4 {
    param([string]$Value)
    $parsed = $null
    if ($Value -notmatch '^([0-9]{1,3}\.){3}[0-9]{1,3}$' -or
        -not [Net.IPAddress]::TryParse($Value, [ref]$parsed) -or
        $parsed.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) {
        throw "Adresse IPv4 invalide : $Value"
    }
    $bytes = $parsed.GetAddressBytes()
    if ($bytes[0] -eq 0 -or $bytes[0] -eq 127 -or $bytes[0] -ge 224) {
        throw "Adresse non utilisable pour cette liaison unicast : $Value"
    }
    return $parsed.ToString()
}

if ([string]::IsNullOrWhiteSpace($LocalIp)) {
    $LocalIp = if ($Role -eq 'A') { '10.77.0.1' } else { '10.77.0.2' }
}
if ([string]::IsNullOrWhiteSpace($PeerIp)) {
    $PeerIp = if ($Role -eq 'A') { '10.77.0.2' } else { '10.77.0.1' }
}
$LocalIp = ConvertTo-IPv4 $LocalIp
$PeerIp = ConvertTo-IPv4 $PeerIp
if ($LocalIp -eq $PeerIp) { throw 'Les adresses locale et distante doivent etre differentes.' }

Import-Module NetAdapter,NetTCPIP -ErrorAction Stop
if ($AllowPing) { Import-Module NetSecurity -ErrorAction Stop }

# Stable requested GUID avoids a new arbitrary identity on every normal restart.
$hasher = [Security.Cryptography.SHA256]::Create()
try {
    $hash = $hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes("SerieIpV1|$env:COMPUTERNAME|$AdapterName"))
    $adapterGuid = [Guid]::new([byte[]]($hash[0..15]))
}
finally { $hasher.Dispose() }

$mutex = $null
$ownsMutex = $false
$bridge = $null
$firewallName = $null
$firewallCreated = $false
try {
    $mutex = [Threading.Mutex]::new($false, "Global\SerieIP-$($adapterGuid.ToString('N'))")
    try { $ownsMutex = $mutex.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $ownsMutex = $true }
    if (-not $ownsMutex) { throw "Une autre instance utilise deja $AdapterName." }

    $existingAdapter = @(Get-NetAdapter -IncludeHidden | Where-Object { $_.Name -eq $AdapterName })
    if ($existingAdapter.Count -ne 0) {
        throw "Un adaptateur nomme $AdapterName existe deja. Il ne sera ni modifie ni supprime. Choisir -AdapterName SerieIP2 ou examiner l ancienne instance."
    }
    $existingAddresses = @(Get-NetIPAddress -AddressFamily IPv4 | Where-Object {
        $_.IPAddress -eq $LocalIp -or $_.IPAddress -eq $PeerIp
    })
    if ($existingAddresses.Count -ne 0) {
        throw 'Une des deux adresses est deja attribuee a ce PC. Choisir deux autres adresses libres.'
    }
    $existingRoute = @(Get-NetRoute -AddressFamily IPv4 | Where-Object {
        $_.DestinationPrefix -eq "$PeerIp/32"
    })
    if ($existingRoute.Count -ne 0) {
        throw "Une route specifique vers $PeerIp/32 existe deja. La configuration existante est conservee."
    }

    Write-Host "DLL locale : $dll"
    Write-Host "Signature  : $($signature.SignerCertificate.Subject)"
    Write-Host "Ouverture $Port : $BaudRate bauds, 8N1, XON/XOFF."
    Write-Host "Echappement : $EscapeMode ; file PC : $QueuePackets paquets."
    Write-Host 'Le cycle radio, les buffers du modem et les delais USB ne sont pas modifies.'
    if ($EscapeMode -eq 'Minimal') {
        Write-Host 'Minimal suppose un chemin binaire 8 bits transparent sauf XON/XOFF, comme AirModem 0.3.0.'
    }
    $bridge = [SerieIpAirModemV2.Bridge]::new(
        $dll, $AdapterName, $adapterGuid, $Port.ToUpperInvariant(), $BaudRate, $Mtu,
        $QueuePackets, ($WriteTimeoutSeconds * 1000), (-not $NoDtr), (-not $NoRts),
        $LocalIp, $PeerIp, ($EscapeMode -eq 'Minimal')
    )
    $ifIndex = $bridge.InterfaceIndex

    # Wait for the newly created IP interface to appear in CIM.
    $ipInterface = $null
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        $ipInterface = Get-NetIPInterface -InterfaceIndex $ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue
        if ($null -ne $ipInterface) { break }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $ipInterface) { throw 'La nouvelle interface IPv4 ne devient pas disponible.' }

    # Only this newly created interface is changed. /32 + a single peer route
    # intentionally keeps unrelated hosts and the default route off the serial link.
    Set-NetIPInterface -InterfaceIndex $ifIndex -AddressFamily IPv4 `
        -Dhcp Disabled -NlMtuBytes $Mtu -AutomaticMetric Disabled -InterfaceMetric 5000 `
        -Forwarding Disabled -DadTransmits 0 -PolicyStore ActiveStore | Out-Null
    New-NetIPAddress -InterfaceIndex $ifIndex -AddressFamily IPv4 -IPAddress $LocalIp `
        -PrefixLength 32 -SkipAsSource $false -PolicyStore ActiveStore | Out-Null
    New-NetRoute -InterfaceIndex $ifIndex -AddressFamily IPv4 -DestinationPrefix "$PeerIp/32" `
        -NextHop '0.0.0.0' -RouteMetric 1 -PolicyStore ActiveStore | Out-Null
    if (Get-Command Set-DnsClient -ErrorAction SilentlyContinue) {
        try {
            Set-DnsClient -InterfaceIndex $ifIndex -RegisterThisConnectionsAddress $false `
                -UseSuffixWhenRegistering $false | Out-Null
        }
        catch { Write-Warning "Inscription DNS non ajustee : $($_.Exception.Message)" }
    }

    if ($AllowPing) {
        # A single narrow inbound rule, removed in finally. No firewall disable.
        # PersistentStore is used explicitly; forced process termination can leave
        # this rule behind. Its unique name is printed for manual inspection.
        $firewallName = 'SerieIP-Ping-' + [Guid]::NewGuid().ToString('N')
        New-NetFirewallRule -Name $firewallName -DisplayName "SerieIP ping $PeerIp vers $LocalIp" `
            -Description 'Regle creee par Serie-IP-AirModem.ps1, retiree a l arret normal.' `
            -InterfaceAlias $AdapterName -LocalAddress $LocalIp -RemoteAddress $PeerIp `
            -Direction Inbound -Protocol ICMPv4 -IcmpType 8 -Action Allow -Profile Any `
            -PolicyStore PersistentStore | Out-Null
        $firewallCreated = $true
        Write-Host "Regle ping : $firewallName"
    }

    $bridge.Start()
    Write-Host ''
    Write-Host "Passerelle locale active : $LocalIp <-> $PeerIp ; MTU $Mtu ; interface $ifIndex."
    Write-Host 'Cette indication ne confirme pas que le PC distant repond.'
    Write-Host 'Arret : Q dans cette console, ou Ctrl+C. Garder le script actif pendant les tests.'
    Write-Host 'IPv4 entre ces deux adresses uniquement ; aucun chiffrement ni partage Internet.'
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $nextStatus = [long]0
    $consoleKeys = ($Host.Name -eq 'ConsoleHost')
    while ($true) {
        $failure = $bridge.Failure
        if (-not [string]::IsNullOrEmpty($failure)) { throw $failure }
        if ($DurationSeconds -gt 0 -and $clock.Elapsed.TotalSeconds -ge $DurationSeconds) { break }
        if ($consoleKeys) {
            try {
                if ([Console]::KeyAvailable) {
                    $key = [Console]::ReadKey($true)
                    if ($key.Key -eq [ConsoleKey]::Q) { break }
                }
            }
            catch { $consoleKeys = $false }
        }
        if ($clock.ElapsedMilliseconds -ge $nextStatus) {
            Write-Host ($bridge.Status())
            Write-Host ($bridge.FrameStatus())
            $nextStatus = $clock.ElapsedMilliseconds + 5000
        }
        Start-Sleep -Milliseconds 200
    }
}
finally {
    # Remove only the rule created by this invocation.
    if ($firewallCreated) {
        try { Remove-NetFirewallRule -Name $firewallName -PolicyStore PersistentStore -ErrorAction Stop }
        catch { Write-Warning "Regle a examiner/supprimer manuellement : $firewallName. $($_.Exception.Message)" }
    }
    if ($null -ne $bridge) {
        try {
            $bridge.Dispose()
            Write-Host ($bridge.Status())
            Write-Host ($bridge.FrameStatus())
            Write-Host 'Session fermee ; adaptateur cree par cette instance retire. Le paquet du pilote Wintun peut rester installe.'
        }
        catch { Write-Warning "Nettoyage incomplet : $($_.Exception.Message)" }
    }
    if ($ownsMutex -and $null -ne $mutex) { $mutex.ReleaseMutex() }
    if ($null -ne $mutex) { $mutex.Dispose() }
}
