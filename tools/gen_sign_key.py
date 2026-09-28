#!/usr/bin/env python3
"""One-off generator of the Android debug signing key for Ksila.

Creates android/signing/key.pk8 (PKCS#8 DER) and cert.x509.pem (self-signed).
This is a DEBUG key committed on purpose so every CI build produces an APK
with the same signature (updates install over previous builds without
uninstalling). Do NOT use it for Play Store releases.

Requires: pip install cryptography
"""

import datetime
import os

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

OUT_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "android", "signing"))


def main():
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, "Ksila Debug"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, "Ksila"),
        x509.NameAttribute(NameOID.COUNTRY_NAME, "US"),
    ])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=30 * 365))
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=False)
        .sign(key, hashes.SHA256())
    )

    os.makedirs(OUT_DIR, exist_ok=True)
    key_path = os.path.join(OUT_DIR, "key.pk8")
    with open(key_path, "wb") as f:
        f.write(key.private_bytes(
            serialization.Encoding.DER,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        ))
    cert_path = os.path.join(OUT_DIR, "cert.x509.pem")
    with open(cert_path, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))

    print(f"signing key: {key_path} ({os.path.getsize(key_path)} bytes)")
    print(f"certificate: {cert_path} ({os.path.getsize(cert_path)} bytes)")


if __name__ == "__main__":
    main()
