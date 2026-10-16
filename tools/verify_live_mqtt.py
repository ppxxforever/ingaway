#!/usr/bin/env python3
"""Verify a live MQTT broker enforces mTLS, without restarting it or publishing data."""
import argparse
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import tempfile
import uuid


def connect(host, port, ca, cert=None, key=None):
    context = ssl.create_default_context(cafile=str(ca))
    # TLS 1.2 makes client-certificate rejection observable during handshake.
    context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
    if cert:
        context.load_cert_chain(str(cert), str(key))
    with socket.create_connection((host, port), timeout=10) as tcp:
        with context.wrap_socket(tcp, server_hostname=host) as stream:
            client_id = ("check-" + uuid.uuid4().hex[:16]).encode()
            body = b"\x00\x04MQTT\x04\x02\x00\x1e" + struct.pack("!H", len(client_id)) + client_id
            stream.sendall(bytes([0x10, len(body)]) + body)
            response = b""
            while len(response) < 4:
                chunk = stream.recv(4 - len(response))
                if not chunk:
                    raise EOFError("Broker closed connection before CONNACK")
                response += chunk
            if response != b"\x20\x02\x00\x00":
                raise RuntimeError(f"MQTT CONNECT refused: {response.hex()}")
            stream.sendall(b"\xe0\x00")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="10.30.192.176")
    parser.add_argument("--port", type=int, default=8883)
    parser.add_argument("--pki", type=Path, default=Path(__file__).resolve().parents[1] / ".local/mqtt-pki/current")
    args = parser.parse_args()
    device = args.pki / "device"

    def positive():
        connect(args.host, args.port, device / "ca.crt", device / "client.crt", device / "client.key")

    def rejected(label, cert=None, key=None):
        try:
            connect(args.host, args.port, device / "ca.crt", cert, key)
        except ssl.SSLCertVerificationError:
            raise  # Failure to authenticate the server is not a client rejection.
        except (ssl.SSLError, EOFError, ConnectionResetError):
            positive()  # A dead broker or bad route must not count as a pass.
            print(f"PASS: {label} rejected; valid client still accepted")
        else:
            raise RuntimeError(f"FAIL: {label} accepted")

    positive()
    print("PASS: server identity, TLS 1.2 and MQTT CONNECT (CONNACK=20020000)")
    rejected("missing client certificate")
    with tempfile.TemporaryDirectory(prefix="mqtt-untrusted-") as tmp:
        cert, key = Path(tmp) / "client.crt", Path(tmp) / "client.key"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-sha256", "-days", "1", "-subj", "/CN=untrusted-test-only",
                        "-addext", "extendedKeyUsage=clientAuth",
                        "-keyout", str(key), "-out", str(cert)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        key.chmod(0o600)
        rejected("untrusted client certificate", cert, key)


if __name__ == "__main__":
    main()
