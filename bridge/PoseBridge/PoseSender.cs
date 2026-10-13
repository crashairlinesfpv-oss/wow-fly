using UnityEngine;

[DefaultExecutionOrder(32000)]
public class PoseSender : MonoBehaviour
{
    Rigidbody _body;
    ContactSample[] _contacts;
    float _contactsUntil;

    void Awake()
    {
        _body = GetComponent<Rigidbody>();
    }

    void FixedUpdate()
    {
        if (_body == null)
            _body = GetComponent<Rigidbody>();
        if (_body == null)
            return;

        OpenSky.Apply(_body);
        if (!OpenSky.ShouldPublish)
            return;

        if (!OpenSky.IsFlying && _contacts != null && Time.fixedTime <= _contactsUntil)
            ContactApply.Apply(_body, _contacts);

        PoseBridgePlugin.Publish(_body);
    }

    internal void SetContacts(ContactSample[] contacts)
    {
        _contacts = contacts;
        _contactsUntil = Time.fixedTime + 0.2f;
    }
}
