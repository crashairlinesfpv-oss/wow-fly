using System;
using System.IO;

internal struct PoseSample
{
    public uint Sequence;
    public uint TimeMs;
    public float Px, Py, Pz;
    public float Qx, Qy, Qz, Qw;
    public float Vx, Vy, Vz;
}

internal struct ContactSample
{
    public float Px, Py, Pz;
    public float Nx, Ny, Nz;
    public uint Flags;
}

internal static class PosePacket
{
    public const int PoseSize = 56;
    public const int ContactStride = 28;
    public const int MaxContacts = 4;

    public static byte[] PackPose(PoseSample pose)
    {
        var buf = new byte[PoseSize];
        using (var stream = new MemoryStream(buf))
        using (var w = new BinaryWriter(stream))
        {
            w.Write((byte)'W');
            w.Write((byte)'D');
            w.Write((byte)'R');
            w.Write((byte)'N');
            w.Write((byte)1);
            w.Write((byte)1);
            w.Write((ushort)0);
            w.Write(pose.Sequence);
            w.Write(pose.TimeMs);
            w.Write(pose.Px);
            w.Write(pose.Py);
            w.Write(pose.Pz);
            w.Write(pose.Qx);
            w.Write(pose.Qy);
            w.Write(pose.Qz);
            w.Write(pose.Qw);
            w.Write(pose.Vx);
            w.Write(pose.Vy);
            w.Write(pose.Vz);
        }
        return buf;
    }

    public static bool TryUnpackPose(byte[] data, int length, out PoseSample pose)
    {
        pose = new PoseSample();
        if (data == null || length < PoseSize || !Magic(data, length))
            return false;
        if (data[4] != 1 || data[5] != 1)
            return false;
        using (var stream = new MemoryStream(data, 0, length, false))
        using (var r = new BinaryReader(stream))
        {
            stream.Position = 6;
            if (r.ReadUInt16() != 0)
                return false;
            pose.Sequence = r.ReadUInt32();
            pose.TimeMs = r.ReadUInt32();
            pose.Px = r.ReadSingle();
            pose.Py = r.ReadSingle();
            pose.Pz = r.ReadSingle();
            pose.Qx = r.ReadSingle();
            pose.Qy = r.ReadSingle();
            pose.Qz = r.ReadSingle();
            pose.Qw = r.ReadSingle();
            pose.Vx = r.ReadSingle();
            pose.Vy = r.ReadSingle();
            pose.Vz = r.ReadSingle();
        }
        return true;
    }

    public static bool TryUnpackContacts(byte[] data, int length, out ContactSample[] contacts)
    {
        contacts = null;
        if (data == null || length < 8 || !Magic(data, length))
            return false;
        if (data[4] != 1 || data[5] != 2 || data[7] != 0)
            return false;
        int count = data[6];
        if (count > MaxContacts || length < 8 + count * ContactStride)
            return false;
        var list = new ContactSample[count];
        using (var stream = new MemoryStream(data, 0, length, false))
        using (var r = new BinaryReader(stream))
        {
            stream.Position = 8;
            for (int i = 0; i < count; i++)
            {
                list[i].Px = r.ReadSingle();
                list[i].Py = r.ReadSingle();
                list[i].Pz = r.ReadSingle();
                list[i].Nx = r.ReadSingle();
                list[i].Ny = r.ReadSingle();
                list[i].Nz = r.ReadSingle();
                list[i].Flags = r.ReadUInt32();
            }
        }
        contacts = list;
        return true;
    }

    static bool Magic(byte[] data, int length)
    {
        return length >= 4 && data[0] == (byte)'W' && data[1] == (byte)'D' && data[2] == (byte)'R' && data[3] == (byte)'N';
    }
}
