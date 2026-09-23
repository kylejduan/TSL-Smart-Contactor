# Static application hosting on Vercel

Deploy the repository's `hosting/` directory as a separate static Vercel project.
This can be done before creating the Tesla application: the initial site requires
no client ID, secret, public key, device connection or firmware build.

## Initial DNS and HTTPS

1. Import this GitHub repository into a new Vercel project. Set **Root Directory**
   to `hosting`, framework **Other**, build/install commands empty and output
   directory `.`. The checked-in `vercel.json` supplies the static configuration.
2. Attach the chosen application hostname as a production domain. Use a hostname
   without `tesla` in its name; the Tesla registration form rejects that word.
3. At the authoritative DNS provider, add the CNAME target displayed by Vercel
   for that exact hostname. For Cloudflare, use **DNS only**, not Proxied. Do not
   alter the personal website's apex/www records or mail records.
4. Wait for public DNS resolution and Vercel certificate issuance. Verify the
   landing page and `/callback.html` from a signed-out browser with normal TLS
   verification. Both must return HTTP 200 without login/challenge pages. The
   callback path must remain `/callback.html`; do not enable `cleanUrls`.
5. Keep the production domain publicly accessible. Preview deployments may remain
   protected. Leave analytics, injected scripts and log drains off this project.

For the owner's planned hostname, the Tesla application entries are:

| Field | Value |
| --- | --- |
| Allowed Origin URL | `https://charge.kylejduan.com` |
| Allowed Redirect URL | `https://charge.kylejduan.com/callback.html` |
| Allowed Return URL | Leave blank if optional; the helper does not use it |

This public hostname is separate from the controller's private LAN address and
per-device certificate. Submit domain entries only after DNS and HTTPS checks pass.

### Deployment verified 2026-09-23

- Vercel project: `tsl-onboarding` in team `kylejduan-projects`; production branch
  `main`, repository root directory `hosting`, framework Other, no install/build
  command, output directory `.`. The existing personal website is a separate project.
- Production deployment of commit `5d41138` reached **READY**. The landing page and
  `/callback.html` returned HTTP 200 and exactly matched their committed files.
- The owner added the Cloudflare **DNS-only CNAME** `charge` pointing to
  `2c8a7b35203bd29e.vercel-dns-017.com`. Authoritative Cloudflare DNS and a public
  recursive resolver returned this record; Vercel verified domain ownership and
  reported `configured-correctly` with no conflicts.
- Custom-domain HTTPS for `/callback.html` passed normal certificate-chain and
  hostname checks and returned the exact callback bytes with `Cache-Control:
  no-store`, `Referrer-Policy: no-referrer` and the restrictive CSP. The first check
  used a currently resolved Vercel IP because the workstation retained a negative
  DNS lookup from before the record existed; certificate verification was enabled.
- No Tesla application acceptance, public-key generation/hosting, consent, token
  exchange, USB provisioning or relay operation was performed by this hosting step.
  At that checkpoint the public-key URL was unavailable; see the subsequent key
  deployment below.

The Vercel CLI is authenticated locally; its `.vercel/` metadata and generated
`.env*` files are ignored. They must never be committed or copied into site output.

### Public key verified 2026-09-23

After local preparation, only the generated EC P-256 **public** key was copied to
the permitted hosting path. Production commit `40b1271` reached **READY** on Vercel.
The custom-domain key URL returned HTTP 200 with normal certificate verification
and no redirect, exactly matching the local public PEM bytes. SHA-256:
`c1252490029f832d10090704ba50a0f2157cc2e1bfc40df923c0cffb29ae5bb0`.
The callback also returned the exact committed bytes. Private keys, client metadata
and device certificates stayed local. This verifies key availability, not Tesla
regional registration or consent, which require the owner's local helper session.

## After Tesla assigns a client ID

Run the [README preparation helper](../README.md#one-time-tesladomain-setup) with
the same application hostname and exact redirect. It generates keys locally.
Create `hosting/.well-known/appspecific/` and copy only
`provisioning/public/.well-known/appspecific/com.tesla.3p.public-key.pem` there.
The repository ignore rules permit that single **public** key path. Review and
commit only the public key, then deploy the updated static site. Do not publish
`provisioning/`, `private/`, device certificates, setup metadata or any client secret.

Verify the deployed key is HTTP 200, contains a PEM **PUBLIC KEY**, and matches the
locally generated public key exactly before regional partner registration. Keep
it hosted. The callback is already provided in `hosting/`; no server, function,
JavaScript or Vercel environment variable is required for OAuth token exchange.

The HTTPS host necessarily receives the callback request and its short-lived
authorization code. `no-store` does not guarantee provider-side log erasure.
The page does not read or transmit the query string; paste it only into the
local helper's hidden prompt.

References: [Vercel domains](https://vercel.com/docs/domains/working-with-domains/add-a-domain),
[Vercel SSL](https://vercel.com/docs/domains/working-with-ssl),
[Vercel static configuration](https://vercel.com/docs/project-configuration/vercel-json),
[Tesla registration](https://developer.tesla.com/docs/fleet-api/endpoints/partner-endpoints).
