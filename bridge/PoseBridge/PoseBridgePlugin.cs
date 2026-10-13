using System;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using BepInEx;
using BepInEx.Configuration;
using BepInEx.Logging;
using UnityEngine;

[BepInPlugin(PluginGuid, PluginName, PluginVersion)]
public class PoseBridgePlugin : BaseUnityPlugin
{
    public const string PluginGuid = "com.crash.velocidrone.posebridge";
    public const string PluginName = "PoseBridge";
    public const string PluginVersion = "0.1.15";

    static ManualLogSource Log;
    static ConfigEntry<bool> Enabled;
    static ConfigEntry<string> RemoteHost;
    static ConfigEntry<int> RemotePort;
    static ConfigEntry<bool> CameraEnabled;
    static ConfigEntry<string> CameraHost;
    static ConfigEntry<int> CameraPort;
    static ConfigEntry<bool> OpenSkyFlight;
    static ConfigEntry<bool> Collisions;
    static ConfigEntry<float> BodyRadius;
    static ConfigEntry<float> CrashSpeed;
    static ConfigEntry<float> CrashSeconds;
    static ConfigEntry<float> EyeHeight;

    public static bool OpenSkyEnabled
    {
        get { return OpenSkyFlight == null || OpenSkyFlight.Value; }
    }

    public static bool CollisionsEnabled
    {
        get { return Collisions == null || Collisions.Value; }
    }

    public static float BodyRadiusValue
    {
        get { return BodyRadius == null ? 0.2f : Mathf.Max(0.02f, BodyRadius.Value); }
    }

    public static float CrashSpeedValue
    {
        get { return CrashSpeed == null ? 8f : CrashSpeed.Value; }
    }

    public static float CrashSecondsValue
    {
        get { return CrashSeconds == null ? 1.5f : CrashSeconds.Value; }
    }

    public static float EyeHeightValue
    {
        get { return EyeHeight == null ? 1.6f : Mathf.Max(0f, EyeHeight.Value); }
    }

    const int TogglePort = 47563;

    static UdpClient _udp;
    static UdpClient _cameraUdp;
    static UdpClient _toggleUdp;
    static IPEndPoint _remote;
    static uint _sequence;
    static int _sentThisWindow;
    static float _windowStart;
    static bool _socketFailed;
    static float _nextScan;

    void Awake()
    {
        Log = Logger;
        Enabled = Config.Bind("General", "Enabled", true, "Publish the quad rigidbody pose.");
        RemoteHost = Config.Bind("Network", "RemoteHost", "127.0.0.1", "Worldserver address that should receive the pose.");
        RemotePort = Config.Bind("Network", "RemotePort", 47561, "Pose destination port.");
        CameraEnabled = Config.Bind("Network", "CameraEnabled", true, "Also send the pose to the local FPV camera.");
        CameraHost = Config.Bind("Network", "CameraHost", "127.0.0.1", "Local camera host. Must stay on this machine.");
        CameraPort = Config.Bind("Network", "CameraPort", 47562, "Local camera port. Same type 1 datagram as the server.");
        OpenSkyFlight = Config.Bind("Flight", "OpenSky", true, "Ignore Velocidrone scenery while flying so the pose is not stopped by that map.");
        Collisions = Config.Bind("Collision", "Enabled", true, "While flying, stop at the WoW walls, buildings and ground the worldserver sends.");
        BodyRadius = Config.Bind("Collision", "BodyRadius", 0.2f, "Metres the quad keeps between its centre and a WoW surface.");
        CrashSpeed = Config.Bind("Collision", "CrashSpeed", 8f, "Metres/second straight into a surface that counts as a crash. Slower touches slide. 0 never crashes.");
        CrashSeconds = Config.Bind("Collision", "CrashSeconds", 1.5f, "After a crash the quad ignores the motors and drops for this many seconds.");
        EyeHeight = Config.Bind("Collision", "EyeHeight", 1.6f, "Yards above the quad where walls are tested. Match drone_fpv.txt height. The ground test stays on the quad.");
        _windowStart = Time.unscaledTime;
        Application.runInBackground = true;
        AllowBackgroundRadio();
        Log.LogInfo("PoseBridge " + PluginVersion + " -> " + RemoteHost.Value + ":" + RemotePort.Value
            + " camera " + CameraHost.Value + ":" + CameraPort.Value
            + " collision " + (CollisionsEnabled ? "on" : "off") + " (F8 toggles)");
        EnsureToggleSocket();
    }

    void Update()
    {
        if (!Application.runInBackground)
            Application.runInBackground = true;
        AllowBackgroundRadio();
        if (Time.unscaledTime >= _nextScan)
        {
            _nextScan = Time.unscaledTime + 0.5f;
            RefreshFocusHolders();
            if (Enabled != null && Enabled.Value)
                AttachToLocalQuad();
        }
        HoldRadioFocus();
        if (Input.GetKeyDown(KeyCode.F8))
            ToggleCollisions();
        PollToggle();
        if (!Enabled.Value)
            return;
    }

    void OnDestroy()
    {
        if (_udp != null)
        {
            _udp.Close();
            _udp = null;
        }
        if (_cameraUdp != null)
        {
            _cameraUdp.Close();
            _cameraUdp = null;
        }
    }

    static bool _radioAllowed;
    static bool _loggedHold;
    static MonoBehaviour[] _focusHolders = new MonoBehaviour[0];

    static void RefreshFocusHolders()
    {
        MonoBehaviour[] all = UnityEngine.Object.FindObjectsOfType<MonoBehaviour>();
        int n = 0;
        for (int i = 0; i < all.Length; i++)
        {
            if (all[i] == null)
                continue;
            string name = all[i].GetType().Name;
            if (name == "Receiver" || name == "VDroneCtrl")
                n++;
        }
        var found = new MonoBehaviour[n];
        int w = 0;
        for (int i = 0; i < all.Length; i++)
        {
            if (all[i] == null)
                continue;
            string name = all[i].GetType().Name;
            if (name == "Receiver" || name == "VDroneCtrl")
                found[w++] = all[i];
        }
        _focusHolders = found;
    }

    static void HoldRadioFocus()
    {
        for (int i = 0; i < _focusHolders.Length; i++)
        {
            MonoBehaviour behaviour = _focusHolders[i];
            if (behaviour == null)
                continue;
            FieldInfo field = behaviour.GetType().GetField("hldadlcjmdo", BindingFlags.Instance | BindingFlags.NonPublic);
            if (field == null || field.FieldType != typeof(bool))
                continue;
            if (!(bool)field.GetValue(behaviour))
                field.SetValue(behaviour, true);
        }
        if (!_loggedHold && _focusHolders.Length > 0)
        {
            _loggedHold = true;
            Log.LogInfo("PoseBridge holding radio input across focus for " + _focusHolders.Length + " behaviours");
        }
    }

    static void AllowBackgroundRadio()
    {
        if (_radioAllowed)
            return;
        Assembly[] asms = AppDomain.CurrentDomain.GetAssemblies();
        for (int i = 0; i < asms.Length; i++)
        {
            Type reinput = asms[i].GetType("Rewired.ReInput");
            if (reinput == null)
                continue;
            PropertyInfo configProp = reinput.GetProperty("configuration", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
            if (configProp == null)
                return;
            object config = configProp.GetValue(null, null);
            if (config == null)
                return;
            PropertyInfo ignore = config.GetType().GetProperty("ignoreInputWhenAppNotInFocus", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            if (ignore == null || ignore.PropertyType != typeof(bool) || !ignore.CanWrite)
                return;
            ignore.SetValue(config, false, null);
            _radioAllowed = true;
            Log.LogInfo("PoseBridge radio stays on when the window is in the background");
            return;
        }
    }

    static void AttachToLocalQuad()
    {
        MonoBehaviour[] behaviours = UnityEngine.Object.FindObjectsOfType<MonoBehaviour>();
        for (int i = 0; i < behaviours.Length; i++)
        {
            MonoBehaviour behaviour = behaviours[i];
            if (behaviour == null || behaviour.GetType().Name != "VDroneCtrl")
                continue;
            if (!IsLocal(behaviour))
                continue;
            if (behaviour.GetComponent<Rigidbody>() == null)
                continue;
            if (behaviour.GetComponent<PoseSender>() == null)
                behaviour.gameObject.AddComponent<PoseSender>();
        }
    }

    static bool IsLocal(MonoBehaviour behaviour)
    {
        PropertyInfo viewProp = behaviour.GetType().GetProperty("photonView");
        if (viewProp == null)
            return true;
        object view = viewProp.GetValue(behaviour, null);
        if (view == null)
            return true;
        PropertyInfo mineProp = view.GetType().GetProperty("IsMine");
        if (mineProp == null)
            return true;
        object mine = mineProp.GetValue(view, null);
        if (mine is bool)
            return (bool)mine;
        return true;
    }

    static bool _cameraWarned;

    static void SendCamera(byte[] payload)
    {
        if (CameraEnabled == null || !CameraEnabled.Value || payload == null)
            return;
        try
        {
            if (_cameraUdp == null)
                _cameraUdp = new UdpClient();
            _cameraUdp.Send(payload, payload.Length, CameraHost.Value, CameraPort.Value);
            _cameraWarned = false;
        }
        catch (SocketException)
        {
            if (!_cameraWarned)
            {
                _cameraWarned = true;
                Log.LogInfo("PoseBridge camera port has no listener yet");
            }
        }
        catch (Exception ex)
        {
            if (!_cameraWarned)
            {
                _cameraWarned = true;
                Log.LogWarning("PoseBridge camera send failed: " + ex.Message);
            }
        }
    }

    public static void Publish(Rigidbody body)
    {
        if (Enabled == null || !Enabled.Value || body == null)
            return;
        Vector3 world = OpenSky.WorldPosition(body);
        if (float.IsNaN(world.x) || float.IsNaN(body.rotation.x))
            return;

        EnsureSocket();
        if (_udp == null)
            return;

        PollContacts(body);

        PoseSample pose = new PoseSample();
        pose.Sequence = ++_sequence;
        pose.TimeMs = (uint)Mathf.Max(0f, Time.fixedTime * 1000f);
        pose.Px = world.x;
        pose.Py = world.y;
        pose.Pz = world.z;
        pose.Qx = body.rotation.x;
        pose.Qy = body.rotation.y;
        pose.Qz = body.rotation.z;
        pose.Qw = body.rotation.w;
        pose.Vx = body.velocity.x;
        pose.Vy = body.velocity.y;
        pose.Vz = body.velocity.z;

        byte[] payload = PosePacket.PackPose(pose);
        try
        {
            _udp.Send(payload, payload.Length);
        }
        catch (Exception ex)
        {
            if (!_socketFailed)
            {
                _socketFailed = true;
                Log.LogWarning("PoseBridge send failed: " + ex.Message);
            }
            SendCamera(payload);
            return;
        }
        SendCamera(payload);

        _sentThisWindow++;
        float now = Time.unscaledTime;
        if (now - _windowStart >= 1f)
        {
            Log.LogInfo("PoseBridge " + _sentThisWindow + " pkt/s seq=" + _sequence);
            _sentThisWindow = 0;
            _windowStart = now;
        }
    }

    static void ToggleCollisions()
    {
        if (Collisions == null)
            return;
        Collisions.Value = !Collisions.Value;
        if (!Collisions.Value)
            ContactApply.Reset();
        Log.LogInfo(Collisions.Value ? "PoseBridge collisions on" : "PoseBridge collisions off");
    }

    static void EnsureToggleSocket()
    {
        if (_toggleUdp != null)
            return;
        try
        {
            _toggleUdp = new UdpClient(new IPEndPoint(IPAddress.Loopback, TogglePort));
            _toggleUdp.Client.Blocking = false;
        }
        catch (Exception ex)
        {
            Log.LogError("PoseBridge toggle socket failed: " + ex.Message);
            if (_toggleUdp != null)
            {
                _toggleUdp.Close();
                _toggleUdp = null;
            }
        }
    }

    static void PollToggle()
    {
        EnsureToggleSocket();
        if (_toggleUdp == null)
            return;
        for (int i = 0; i < 8 && _toggleUdp.Available > 0; i++)
        {
            IPEndPoint from = new IPEndPoint(IPAddress.Loopback, 0);
            byte[] data;
            try
            {
                data = _toggleUdp.Receive(ref from);
            }
            catch (SocketException)
            {
                break;
            }
            if (data != null && data.Length >= 7
                && data[0] == (byte)'W' && data[1] == (byte)'D' && data[2] == (byte)'R' && data[3] == (byte)'N'
                && data[4] == 1 && data[5] == 3)
                ToggleCollisions();
        }
    }

    static void PollContacts(Rigidbody body)
    {
        if (_udp == null)
            return;
        ContactSample[] latest = null;
        for (int i = 0; i < 32 && _udp.Available > 0; i++)
        {
            IPEndPoint from = _remote;
            byte[] data;
            try
            {
                data = _udp.Receive(ref from);
            }
            catch (SocketException)
            {
                break;
            }
            ContactSample[] contacts;
            if (PosePacket.TryUnpackContacts(data, data.Length, out contacts))
                latest = contacts;
        }
        if (latest == null)
            return;
        PoseSender sender = body.GetComponent<PoseSender>();
        if (sender != null)
            sender.SetContacts(latest);
    }

    static void EnsureSocket()
    {
        if (_udp != null || _socketFailed)
            return;
        try
        {
            _remote = new IPEndPoint(IPAddress.Parse(RemoteHost.Value), RemotePort.Value);
            _udp = new UdpClient();
            _udp.Client.Blocking = false;
            _udp.Connect(_remote);
        }
        catch (Exception ex)
        {
            _socketFailed = true;
            Log.LogError("PoseBridge socket failed: " + ex.Message);
            if (_udp != null)
            {
                _udp.Close();
                _udp = null;
            }
        }
    }
}
