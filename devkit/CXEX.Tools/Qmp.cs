// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;

namespace CXEX.Tools;

/// <summary>The little of QEMU's machine protocol a verification boot needs.</summary>
public static class Qmp
{
    /// <summary>A local TCP port nothing is listening on right now.</summary>
    public static int FreePort()
    {
        var l = new TcpListener(IPAddress.Loopback, 0);
        l.Start();
        try { return ((IPEndPoint)l.LocalEndpoint).Port; }
        finally { l.Stop(); }
    }

    /// <summary>Writes the screen to <paramref name="path"/> as a PPM. Throws when QEMU refuses or stops answering.</summary>
    public static void Screendump(int port, string path, TimeSpan timeout)
    {
        using var tcp = new TcpClient();
        tcp.Connect(IPAddress.Loopback, port);
        tcp.ReceiveTimeout = (int)timeout.TotalMilliseconds;
        tcp.SendTimeout = (int)timeout.TotalMilliseconds;
        using var stream = tcp.GetStream();
        using var reader = new StreamReader(stream, new UTF8Encoding(false));
        using var writer = new StreamWriter(stream, new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\n" };

        _ = reader.ReadLine();     // the greeting
        Command(reader, writer, new { execute = "qmp_capabilities" });
        Command(reader, writer, new { execute = "screendump", arguments = new { filename = path } });
    }

    private static void Command(StreamReader reader, StreamWriter writer, object command)
    {
        writer.WriteLine(JsonSerializer.Serialize(command));
        while (true)
        {
            string line = reader.ReadLine() ?? throw new IOException("QEMU closed QMP before answering");
            using var doc = JsonDocument.Parse(line);
            if (doc.RootElement.TryGetProperty("error", out var error))
                throw new IOException("QMP: " + error.GetRawText());
            if (doc.RootElement.TryGetProperty("return", out _)) return;
            // anything else is an asynchronous event
        }
    }
}
