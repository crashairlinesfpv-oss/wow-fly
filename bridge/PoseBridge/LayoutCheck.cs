using System;

internal static class LayoutCheck
{
    static int Main()
    {
        var pose = new PoseSample();
        pose.Sequence = 7;
        pose.TimeMs = 1500;
        pose.Px = 1f;
        pose.Py = 2f;
        pose.Pz = 3f;
        pose.Qx = 0f;
        pose.Qy = 0f;
        pose.Qz = 0f;
        pose.Qw = 1f;
        pose.Vx = 0.5f;
        pose.Vy = 0f;
        pose.Vz = -0.25f;

        byte[] packed = PosePacket.PackPose(pose);
        PoseSample back;
        if (packed.Length != PosePacket.PoseSize || !PosePacket.TryUnpackPose(packed, packed.Length, out back))
            return 1;
        if (back.Sequence != 7 || back.Pz != 3f || back.Vz != -0.25f)
            return 1;

        var raw = new byte[8 + PosePacket.ContactStride];
        raw[0] = (byte)'W';
        raw[1] = (byte)'D';
        raw[2] = (byte)'R';
        raw[3] = (byte)'N';
        raw[4] = 1;
        raw[5] = 2;
        raw[6] = 1;
        raw[7] = 0;
        WriteF(raw, 8, 1f);
        WriteF(raw, 12, 2f);
        WriteF(raw, 16, 3f);
        WriteF(raw, 20, 0f);
        WriteF(raw, 24, 1f);
        WriteF(raw, 28, 0f);
        raw[32] = 1;

        ContactSample[] contacts;
        if (!PosePacket.TryUnpackContacts(raw, raw.Length, out contacts) || contacts.Length != 1 || contacts[0].Flags != 1)
            return 1;

        Console.WriteLine("pose " + Hex(packed));
        Console.WriteLine("contact " + Hex(raw));
        return 0;
    }

    static void WriteF(byte[] dest, int offset, float value)
    {
        byte[] bytes = BitConverter.GetBytes(value);
        if (!BitConverter.IsLittleEndian)
            Array.Reverse(bytes);
        Buffer.BlockCopy(bytes, 0, dest, offset, 4);
    }

    static string Hex(byte[] data)
    {
        var chars = new char[data.Length * 2];
        for (int i = 0; i < data.Length; i++)
        {
            byte b = data[i];
            chars[i * 2] = Nibble(b >> 4);
            chars[i * 2 + 1] = Nibble(b & 0xF);
        }
        return new string(chars);
    }

    static char Nibble(int n)
    {
        return (char)(n < 10 ? '0' + n : 'a' + (n - 10));
    }
}
