using UnityEngine;

internal static class ContactApply
{
    const float TouchDistance = 0.5f;
    const float PatchRadius = 3f;
    const float Behind = 1f;
    const float SettleFall = -2f;
    const float MaxFall = -30f;

    static Vector3 _prevWorld;
    static bool _havePrev;
    static float _crashUntil;
    static float _fall;

    public static bool Crashed
    {
        get { return Time.fixedTime < _crashUntil; }
    }

    public static void Apply(Rigidbody body, ContactSample[] contacts)
    {
        if (body == null || contacts == null)
            return;

        for (int i = 0; i < contacts.Length; i++)
        {
            if ((contacts[i].Flags & 1u) == 0)
                continue;

            Vector3 n = new Vector3(contacts[i].Nx, contacts[i].Ny, contacts[i].Nz);
            if (n.sqrMagnitude < 1e-8f)
                continue;
            n.Normalize();

            Vector3 point = new Vector3(contacts[i].Px, contacts[i].Py, contacts[i].Pz);
            float along = Vector3.Dot(body.position - point, n);
            if (along > TouchDistance || along < -TouchDistance)
                continue;
            if (along < 0f)
                OpenSky.Nudge(body, -n * along);

            float vn = Vector3.Dot(body.velocity, n);
            if (vn < 0f)
                body.velocity -= n * vn;
        }
    }

    public static void Reset()
    {
        _havePrev = false;
        _crashUntil = 0f;
        _fall = 0f;
    }

    public static void Fly(Rigidbody body, ContactSample[] contacts, float radius, float crashSpeed, float crashSeconds, float eyeHeight)
    {
        if (body == null)
            return;

        float dt = Time.fixedDeltaTime;
        Vector3 world = OpenSky.WorldPosition(body);
        Vector3 prev = _havePrev ? _prevWorld : world;
        bool landed = false;
        float patch = PatchRadius + body.velocity.magnitude * 0.2f;

        if (Crashed)
        {
            _fall = Mathf.Max(_fall + Physics.gravity.y * dt, MaxFall);
            body.velocity = new Vector3(0f, Mathf.Min(body.velocity.y, _fall), 0f);
        }

        if (contacts != null)
        {
            for (int i = 0; i < contacts.Length; i++)
            {
                if ((contacts[i].Flags & 1u) == 0)
                    continue;

                Vector3 n = new Vector3(contacts[i].Nx, contacts[i].Ny, contacts[i].Nz);
                if (n.sqrMagnitude < 1e-8f)
                    continue;
                n.Normalize();

                Vector3 point = new Vector3(contacts[i].Px, contacts[i].Py, contacts[i].Pz);
                bool floor = n.y > 0.7f;
                Vector3 sample = floor ? world : world + Vector3.up * eyeHeight;
                float now = Vector3.Dot(sample - point, n);
                float vIn = Vector3.Dot(body.velocity, n);
                if (now >= radius && now + vIn * dt >= radius)
                    continue;
                Vector3 prevSample = floor ? prev : prev + Vector3.up * eyeHeight;
                if (Vector3.Dot(prevSample - point, n) < -Behind)
                    continue;
                Vector3 lateral = (sample - point) - n * now;
                if (lateral.magnitude > patch)
                    continue;

                if (now < radius)
                {
                    Vector3 push = n * (radius - now);
                    OpenSky.Nudge(body, push);
                    world += push;
                    sample += push;
                    now = radius;
                }
                if (n.y > 0.7f)
                    landed = true;

                float allowed = -(now - radius) / dt;
                if (vIn >= allowed)
                    continue;
                body.velocity += n * (allowed - vIn);
                float impact = -vIn;
                if (crashSpeed > 0f && impact >= crashSpeed && !Crashed)
                {
                    Vector3 snap = n * (radius - now);
                    OpenSky.Nudge(body, snap);
                    world += snap;
                    sample += snap;
                    StartCrash(body, impact, crashSeconds);
                }
            }
        }

        if (Crashed && landed)
            _fall = SettleFall;

        _prevWorld = world;
        _havePrev = true;
    }

    static void StartCrash(Rigidbody body, float impact, float seconds)
    {
        _crashUntil = Time.fixedTime + Mathf.Max(0f, seconds);
        _fall = 0f;
        body.velocity = Vector3.zero;
        Debug.Log("PoseBridge crash at " + impact.ToString("0.0") + " m/s");
    }
}
