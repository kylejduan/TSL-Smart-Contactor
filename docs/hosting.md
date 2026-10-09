# Static application hosting

The `hosting/` directory provides a generic landing page and static callback with
no scripts, analytics, server or token exchange. Each installation must register
its own application domain and publish its own generated public key. No actual
installation key or domain is shipped by this open-source project.

## Domain and HTTPS

1. Create a static hosting project for `hosting/`, framework Other, no install/build command, output `.`. Keep the existing security headers.
2. Attach your chosen hostname, avoiding `tesla` in the application hostname. Configure the exact DNS target supplied by the host. On Cloudflare use DNS only for the application hostname; preserve unrelated website/mail records.
3. Verify normal HTTPS certificate/hostname validation and HTTP 200 for the landing page and `/callback.html`, without login/challenge pages. Do not enable clean-URL rewriting of the exact callback path.
4. Keep the application domain public, with no analytics, injected scripts or log drains. Preview deployments may remain protected.

| Tesla application field | Example placeholder |
| --- | --- |
| Allowed Origin URL | `https://charge.example.com` |
| Allowed Redirect URL | `https://charge.example.com/callback.html` |
| Allowed Return URL | Leave blank if optional; the helper does not use it |

Replace the reserved example domain with one you control. This application domain
is separate from the controller's LAN HTTPS URL/certificate.

## Installation key and deliberate deployment

Run `tools/onboard.py prepare` with the exact application domain, registered
redirect and device LAN URL. The helper creates keys in ignored `provisioning/`.
Copy only its generated **public** key to
`hosting/.well-known/appspecific/com.tesla.3p.public-key.pem` for your own deployment;
that local file is ignored. Never upload private keys, setup metadata, device
certificates, client secret or the rest of `provisioning/`.

Automatic Git deployment is disabled in `hosting/vercel.json`. This prevents a
public-source push from replacing an installation's privately prepared hosting
output or deleting its application key. Deploy your reviewed local static folder
deliberately using the hosting provider's tools. If you use an Ignored Build Step
to preserve an existing deployment, review that setting before a later manual
release. No firmware release or repository push updates a controller.

Verify the hosted key is HTTP 200 with normal TLS checks, has PEM PUBLIC KEY
content and exactly matches the locally generated public key before regional
registration. Keep it hosted. The callback receives a short-lived OAuth code;
no-store headers do not guarantee host-side log erasure. Paste the full callback
URL only into the local helper's hidden prompt.

References: [Vercel Git configuration](https://vercel.com/docs/project-configuration/git-configuration),
[Vercel domains](https://vercel.com/docs/domains/working-with-domains/add-a-domain),
[Tesla authentication](https://developer.tesla.com/docs/fleet-api/authentication/overview).
