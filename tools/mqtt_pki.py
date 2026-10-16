#!/usr/bin/env python3
"""Generate a private MQTT CA, server identity, and one ESP32 client identity.

Never deploys to a broker. --install-device backs up and replaces local firmware
certificates only. Keys are generated into a new private directory, not stdout.
"""
import argparse
from datetime import datetime, timezone
import ipaddress
import os
from pathlib import Path
import secrets
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    result = subprocess.run(["openssl", *map(str, args)], capture_output=True, check=True)
    return result.stdout


def write(path, data):
    path.write_text(data)
    path.chmod(0o600)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-ip", required=True, type=ipaddress.ip_address)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--install-device", action="store_true")
    args = parser.parse_args()
    os.umask(0o077)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out = (args.out or ROOT / ".local/mqtt-pki" / stamp).resolve()
    out.mkdir(parents=True, exist_ok=False, mode=0o700)
    for name in ("ca", "broker", "device", "previous-device"):
        (out / name).mkdir(mode=0o700)

    ca = out / "ca"
    write(ca / "ca.cnf", f"""[req]
prompt = no
distinguished_name = dn
x509_extensions = v3_ca
[dn]
CN = Ingaway MQTT Root CA {stamp}
[v3_ca]
basicConstraints = critical, CA:true, pathlen:0
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
""")
    run("genrsa", "-traditional", "-out", ca / "ca.key", "3072")
    run("req", "-new", "-x509", "-sha256", "-days", "3650",
        "-key", ca / "ca.key", "-config", ca / "ca.cnf", "-out", ca / "ca.crt")

    for name, cn, purpose in (("server", str(args.server_ip), "serverAuth"),
                              ("client", "esp32-device01", "clientAuth")):
        dest = out / ("broker" if name == "server" else "device")
        extension = ("basicConstraints = critical, CA:false\n"
                     "keyUsage = critical, digitalSignature, keyEncipherment\n"
                     f"extendedKeyUsage = {purpose}\n"
                     "subjectKeyIdentifier = hash\nauthorityKeyIdentifier = keyid,issuer\n")
        if name == "server":
            extension += f"subjectAltName = IP:{args.server_ip}\n"
        write(dest / f"{name}.ext", extension)
        run("genrsa", "-traditional", "-out", dest / f"{name}.key", "2048")
        run("req", "-new", "-sha256", "-key", dest / f"{name}.key",
            "-subj", f"/CN={cn}", "-out", dest / f"{name}.csr")
        run("x509", "-req", "-sha256", "-days", "365", "-in", dest / f"{name}.csr",
            "-CA", ca / "ca.crt", "-CAkey", ca / "ca.key",
            "-set_serial", f"0x{secrets.randbits(159) | 1:x}",
            "-extfile", dest / f"{name}.ext", "-out", dest / f"{name}.crt")
        cert_pub = run("x509", "-in", dest / f"{name}.crt", "-pubkey", "-noout")
        key_pub = run("pkey", "-in", dest / f"{name}.key", "-pubout")
        if cert_pub != key_pub:
            raise RuntimeError(f"{name} certificate/key mismatch")
        shutil.copyfile(ca / "ca.crt", dest / ("client-ca.crt" if name == "server" else "ca.crt"))

    run("verify", "-x509_strict", "-CAfile", ca / "ca.crt", "-purpose", "sslserver",
        "-verify_ip", args.server_ip, out / "broker/server.crt")
    run("verify", "-x509_strict", "-CAfile", ca / "ca.crt", "-purpose", "sslclient",
        out / "device/client.crt")
    write(out / "broker/listeners-ssl.hocon", """# Merge this block into the configuration actually loaded by EMQX Edge.
listeners.ssl {
    enable = true
    bind = "0.0.0.0:8883"
    keyfile = "/opt/emqx-edge/etc/certs/server.key"
    certfile = "/opt/emqx-edge/etc/certs/server.crt"
    cacertfile = "/opt/emqx-edge/etc/certs/client-ca.crt"
    verify_peer = true
    fail_if_no_peer_cert = true
}
""")
    if args.install_device:
        device_dir = ROOT / "components/mymqtt/certs"
        device_dir.mkdir(parents=True, exist_ok=True)
        for filename in ("ca.crt", "client.crt", "client.key"):
            current = device_dir / filename
            if current.exists():
                shutil.copy2(current, out / "previous-device" / filename)
            tmp = device_dir / (filename + ".new")
            shutil.copyfile(out / "device" / filename, tmp)
            tmp.chmod(0o600)
            tmp.replace(current)
    print(f"PKI directory: {out}")
    print("PASS: server IP SAN, strict certificate chains, EKUs, and both key pairs")
    print("Broker files: broker/server.crt, broker/server.key, broker/client-ca.crt")
    print("CA signing key stays in ca/ca.key; do not copy it to the broker or ESP32.")
    if args.install_device:
        print("Firmware certificates installed; original files saved in previous-device/")
    print(run("x509", "-in", ca / "ca.crt", "-noout", "-fingerprint", "-sha256").decode().strip())


if __name__ == "__main__":
    main()
