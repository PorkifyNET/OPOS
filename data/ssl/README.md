# Root certificates

`ca-certificates.crt` is the Mozilla CA bundle (as packaged by Ubuntu's
`ca-certificates` 20260816ubuntu0.24.04.1, 120 roots), licensed under the
Mozilla Public License 2.0. OPOS ships it as
`/etc/ssl/certs/ca-certificates.crt`; `opos_tls_connect` (sdk/lib/tls.c)
trusts the certificates in it. To refresh it, copy the file from an
up-to-date Linux system.
