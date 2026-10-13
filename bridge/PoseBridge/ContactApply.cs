using UnityEngine;

internal static class ContactApply
{
    const float TouchDistance = 0.5f;

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
            // A WoW plane far below the body is not the track pad. Pulling the
            // rigidbody up onto it is what put the reset point in the air.
            if (along > TouchDistance || along < -TouchDistance)
                continue;
            if (along < 0f)
                body.position -= n * along;

            float vn = Vector3.Dot(body.velocity, n);
            if (vn < 0f)
                body.velocity -= n * vn;
        }
    }
}
