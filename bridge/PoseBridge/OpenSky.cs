using System.Reflection;
using UnityEngine;

internal static class OpenSky
{
    const float SettleSpeed = 0.6f;
    const float HoldRadius = 2.5f;
    const float ResetJump = 20f;
    const float CageRadius = 4000f;

    static bool _havePad;
    static Vector3 _pad;
    static Vector3 _offset;
    static Vector3 _prevBody;
    static bool _havePrev;
    static bool _open;
    static bool _hullsReady;
    static Collider[] _hulls;
    static bool[] _hullEnabled;
    static FieldInfo _groundMask;
    static bool _groundMaskReady;
    static int _savedGroundMask;
    static bool _haveGroundMask;
    static bool _boundaryReady;
    static float _boundaryScan;
    static Collider[] _boundary;
    static bool[] _boundaryEnabled;
    static bool _matrixOff;
    static int[] _matrixA;
    static int[] _matrixB;
    static bool[] _matrixIgnored;
    static MonoBehaviour[] _props;
    static int[] _propMasks;
    static bool _propsReady;
    static bool _heldOnTrack;

    public static bool IsFlying
    {
        get { return _open; }
    }

    public static bool ShouldPublish
    {
        get { return _publish; }
    }

    static bool _publish = true;
    static bool _wasArmed = true;

    public static Vector3 WorldPosition(Rigidbody body)
    {
        if (body == null)
            return Vector3.zero;
        return body.position + _offset;
    }

    public static void Apply(Rigidbody body)
    {
        if (body == null)
            return;
        if (!PoseBridgePlugin.OpenSkyEnabled)
        {
            body.detectCollisions = true;
            _offset = Vector3.zero;
            _open = false;
            _publish = true;
            _havePrev = false;
            return;
        }

        if (_havePrev && (body.position - _prevBody).magnitude > ResetJump)
        {
            _offset = Vector3.zero;
            _pad = body.position;
            _havePad = true;
        }

        float speed = body.velocity.magnitude;
        if (!_havePad)
        {
            _pad = body.position;
            _havePad = true;
        }

        Vector3 world = body.position + _offset;
        bool nearHome = (world - _pad).magnitude < HoldRadius;
        bool sitting = !IsArmed(body) || body.isKinematic || (speed < SettleSpeed && nearHome);
        if (sitting)
        {
            if (_open)
                Cage(body);
            if (speed < SettleSpeed && nearHome)
                _pad = world;
            SetSolid(body, true);
            SetBoundary(body, true);
            SetMatrix(body, false);
            if (!IsArmed(body))
                RestOnTrack(body);
            _open = false;
            _publish = false;
            if (_wasArmed)
            {
                _wasArmed = false;
                Debug.Log("PoseBridge quad is sitting; the character can walk");
            }
            _prevBody = body.position;
            _havePrev = true;
            return;
        }
        _publish = true;
        _wasArmed = true;

        SetSolid(body, false);
        SetBoundary(body, false);
        SetMatrix(body, true);
        Cage(body);
        _open = true;

        _prevBody = body.position;
        _havePrev = true;
    }

    static void Cage(Rigidbody body)
    {
        Vector3 delta = body.position - _pad;
        if (delta.sqrMagnitude < CageRadius * CageRadius)
            return;
        Transform root = body.transform.root != null ? body.transform.root : body.transform;
        Rigidbody[] parts = root.GetComponentsInChildren<Rigidbody>(true);
        for (int i = 0; i < parts.Length; i++)
        {
            Rigidbody part = parts[i];
            if (part == null)
                continue;
            Vector3 velocity = part.velocity;
            Vector3 angular = part.angularVelocity;
            part.position -= delta;
            part.velocity = velocity;
            part.angularVelocity = angular;
        }
        _offset += delta;
    }

    static void SetSolid(Rigidbody body, bool solid)
    {
        Transform root = body.transform.root != null ? body.transform.root : body.transform;
        Rigidbody[] bodies = root.GetComponentsInChildren<Rigidbody>(true);
        for (int i = 0; i < bodies.Length; i++)
        {
            if (bodies[i] != null)
                bodies[i].detectCollisions = solid;
        }
        body.detectCollisions = solid;

        Collider[] found = root.GetComponentsInChildren<Collider>(true);
        if (!_hullsReady || found.Length != _hulls.Length)
        {
            bool[] previous = _hullEnabled;
            Collider[] old = _hulls;
            _hulls = found;
            _hullEnabled = new bool[found.Length];
            for (int i = 0; i < found.Length; i++)
            {
                bool known = false;
                if (old != null)
                {
                    for (int j = 0; j < old.Length; j++)
                    {
                        if (old[j] != found[i])
                            continue;
                        _hullEnabled[i] = previous[j];
                        known = true;
                        break;
                    }
                }
                if (!known)
                    _hullEnabled[i] = found[i] != null && (found[i].enabled || !solid);
            }
            _hullsReady = true;
        }
        for (int i = 0; i < _hulls.Length; i++)
        {
            if (_hulls[i] == null)
                continue;
            _hulls[i].enabled = solid && _hullEnabled[i];
        }
        SilenceGroundProbe(body, !solid);
        SilenceProps(body, !solid);
    }

    static void RestOnTrack(Rigidbody body)
    {
        Vector3 origin = body.position + Vector3.up * 40f;
        RaycastHit[] hits = Physics.RaycastAll(origin, Vector3.down, 100f, ~0, QueryTriggerInteraction.Ignore);
        Transform root = body.transform.root;
        float best = float.MaxValue;
        float surface = 0f;
        bool found = false;
        bool supported = false;
        for (int i = 0; i < hits.Length; i++)
        {
            Collider col = hits[i].collider;
            if (col == null)
                continue;
            if (root != null && (col.transform == root || col.transform.IsChildOf(root)))
                continue;
            float rise = hits[i].point.y - body.position.y;
            if (rise >= -0.35f && rise <= 0.15f)
                supported = true;
            if (rise < 0.2f || rise > 60f || rise >= best)
                continue;
            best = rise;
            surface = hits[i].point.y;
            found = true;
        }
        bool deep = _havePad && body.position.y < _pad.y - 2f;
        if (supported || !found || (best > 3f && !deep))
        {
            _heldOnTrack = false;
            return;
        }
        body.position = new Vector3(body.position.x, surface + 0.2f, body.position.z);
        body.velocity = Vector3.zero;
        body.angularVelocity = Vector3.zero;
        if (!_heldOnTrack)
            Debug.Log("PoseBridge put the disarmed quad back on the track");
        _heldOnTrack = true;
    }

    static void SetBoundary(Rigidbody body, bool solid)
    {
        bool scan = !_boundaryReady || _boundary == null || Time.fixedUnscaledTime >= _boundaryScan;
        if (scan)
        {
            Collider[] all = UnityEngine.Object.FindObjectsOfType<Collider>();
            Transform root = body.transform.root;
            int matches = 0;
            for (int i = 0; i < all.Length; i++)
            {
                if (IsScenery(all[i], root))
                    matches++;
            }
            bool[] previous = _boundaryEnabled;
            Collider[] old = _boundary;
            _boundary = new Collider[matches];
            _boundaryEnabled = new bool[matches];
            int w = 0;
            for (int i = 0; i < all.Length; i++)
            {
                if (!IsScenery(all[i], root))
                    continue;
                Collider col = all[i];
                bool known = false;
                bool wasOn = false;
                if (old != null)
                {
                    for (int j = 0; j < old.Length; j++)
                    {
                        if (old[j] != col)
                            continue;
                        wasOn = previous[j];
                        known = true;
                        break;
                    }
                }
                if (!known)
                    wasOn = col.enabled || !solid;
                _boundary[w] = col;
                _boundaryEnabled[w] = wasOn;
                w++;
            }
            if (!_boundaryReady && !solid)
                Debug.Log("PoseBridge scenery colliders off " + matches);
            _boundaryReady = true;
            _boundaryScan = Time.fixedUnscaledTime + 0.05f;
        }
        if (_boundary == null)
            return;
        for (int i = 0; i < _boundary.Length; i++)
        {
            if (_boundary[i] == null)
                continue;
            _boundary[i].enabled = solid && _boundaryEnabled[i];
        }
    }

    static bool IsScenery(Collider col, Transform droneRoot)
    {
        if (col == null || col.transform == null)
            return false;
        if (droneRoot != null && (col.transform == droneRoot || col.transform.IsChildOf(droneRoot)))
            return false;
        return true;
    }

    static void SetMatrix(Rigidbody body, bool ghost)
    {
        if (!ghost)
        {
            if (!_matrixOff)
                return;
            for (int i = 0; i < _matrixA.Length; i++)
                Physics.IgnoreLayerCollision(_matrixA[i], _matrixB[i], _matrixIgnored[i]);
            _matrixOff = false;
            return;
        }
        if (_matrixOff)
            return;
        Transform root = body.transform.root != null ? body.transform.root : body.transform;
        Collider[] cols = root.GetComponentsInChildren<Collider>(true);
        bool[] seen = new bool[32];
        seen[body.gameObject.layer] = true;
        for (int i = 0; i < cols.Length; i++)
        {
            if (cols[i] != null)
                seen[cols[i].gameObject.layer] = true;
        }
        int layers = 0;
        for (int i = 0; i < 32; i++)
            if (seen[i])
                layers++;
        _matrixA = new int[layers * 32];
        _matrixB = new int[layers * 32];
        _matrixIgnored = new bool[layers * 32];
        int w = 0;
        for (int layer = 0; layer < 32; layer++)
        {
            if (!seen[layer])
                continue;
            for (int other = 0; other < 32; other++)
            {
                _matrixA[w] = layer;
                _matrixB[w] = other;
                _matrixIgnored[w] = Physics.GetIgnoreLayerCollision(layer, other);
                Physics.IgnoreLayerCollision(layer, other, true);
                w++;
            }
        }
        _matrixOff = true;
        Debug.Log("PoseBridge flight ignores scenery collisions");
    }

    static bool IsArmed(Rigidbody body)
    {
        MonoBehaviour[] behaviours = body.GetComponents<MonoBehaviour>();
        for (int i = 0; i < behaviours.Length; i++)
        {
            MonoBehaviour behaviour = behaviours[i];
            if (behaviour == null || behaviour.GetType().Name != "VDroneCtrl")
                continue;
            FieldInfo useBetaFlight = behaviour.GetType().GetField("useBetaFlight");
            if (useBetaFlight != null && useBetaFlight.FieldType == typeof(bool) && !(bool)useBetaFlight.GetValue(behaviour))
                return true;
            FieldInfo bfField = behaviour.GetType().GetField("bf");
            if (bfField == null)
                return true;
            object bf = bfField.GetValue(behaviour);
            if (bf == null)
                return true;
            FieldInfo armed = bf.GetType().GetField("armed");
            if (armed == null || armed.FieldType != typeof(bool))
                return true;
            return (bool)armed.GetValue(bf);
        }
        return true;
    }

    static void SilenceGroundProbe(Rigidbody body, bool silent)
    {
        if (!_groundMaskReady)
        {
            MonoBehaviour[] behaviours = body.GetComponents<MonoBehaviour>();
            for (int i = 0; i < behaviours.Length; i++)
            {
                if (behaviours[i] == null || behaviours[i].GetType().Name != "VDroneCtrl")
                    continue;
                _groundMask = behaviours[i].GetType().GetField("ginoebllilg", BindingFlags.Instance | BindingFlags.NonPublic);
                if (_groundMask == null)
                    break;
                object current = _groundMask.GetValue(behaviours[i]);
                if (current is LayerMask)
                {
                    _savedGroundMask = ((LayerMask)current).value;
                    _haveGroundMask = true;
                }
                break;
            }
            _groundMaskReady = true;
        }
        if (_groundMask == null || !_haveGroundMask)
            return;
        MonoBehaviour[] again = body.GetComponents<MonoBehaviour>();
        for (int i = 0; i < again.Length; i++)
        {
            if (again[i] == null || again[i].GetType().Name != "VDroneCtrl")
                continue;
            _groundMask.SetValue(again[i], silent ? (LayerMask)0 : (LayerMask)_savedGroundMask);
            return;
        }
    }

    static void SilenceProps(Rigidbody body, bool silent)
    {
        Transform root = body.transform.root != null ? body.transform.root : body.transform;
        MonoBehaviour[] all = root.GetComponentsInChildren<MonoBehaviour>(true);
        int count = 0;
        for (int i = 0; i < all.Length; i++)
        {
            if (all[i] != null && all[i].GetType().Name == "PropUnit")
                count++;
        }
        if (!_propsReady || _props == null || _props.Length != count)
        {
            MonoBehaviour[] old = _props;
            int[] oldMasks = _propMasks;
            _props = new MonoBehaviour[count];
            _propMasks = new int[count];
            int w = 0;
            for (int i = 0; i < all.Length; i++)
            {
                if (all[i] == null || all[i].GetType().Name != "PropUnit")
                    continue;
                FieldInfo maskField = all[i].GetType().GetField("ginoebllilg", BindingFlags.Instance | BindingFlags.NonPublic);
                int mask = 0;
                if (maskField != null)
                {
                    object current = maskField.GetValue(all[i]);
                    if (current is LayerMask)
                        mask = ((LayerMask)current).value;
                }
                if (old != null)
                {
                    for (int j = 0; j < old.Length; j++)
                    {
                        if (old[j] != all[i])
                            continue;
                        mask = oldMasks[j];
                        break;
                    }
                }
                _props[w] = all[i];
                _propMasks[w] = mask;
                w++;
            }
            _propsReady = true;
        }
        for (int i = 0; i < _props.Length; i++)
        {
            if (_props[i] == null)
                continue;
            FieldInfo maskField = _props[i].GetType().GetField("ginoebllilg", BindingFlags.Instance | BindingFlags.NonPublic);
            if (maskField == null)
                continue;
            maskField.SetValue(_props[i], silent ? (LayerMask)0 : (LayerMask)_propMasks[i]);
        }
    }
}
