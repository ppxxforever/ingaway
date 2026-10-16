#!/usr/bin/env python3
"""Verify generated identities with a temporary localhost TLS 1.2 server.
This does not connect to or reconfigure the actual MQTT broker.
"""
import argparse
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import threading


def exchange(root, cert, key, expected_name="10.30.192.176"):
    server = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    server.minimum_version = server.maximum_version = ssl.TLSVersion.TLSv1_2
    server.load_cert_chain(root / "broker/server.crt", root / "broker/server.key")
    server.load_verify_locations(root / "broker/client-ca.crt")
    server.verify_mode = ssl.CERT_REQUIRED
    result = {}
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(5)
        port = listener.getsockname()[1]

        def serve():
            try:
                raw, _ = listener.accept()
                with raw:
                    raw.settimeout(5)
                    with server.wrap_socket(raw, server_side=True) as secure:
                        secure.sendall(b"verified")
            except Exception as exc:
                result["server_error"] = exc

        thread = threading.Thread(target=serve)
        thread.start()
        client = ssl.create_default_context(cafile=root / "device/ca.crt")
        client.minimum_version = client.maximum_version = ssl.TLSVersion.TLSv1_2
        if cert is not None:
            client.load_cert_chain(cert, key)
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=5) as raw:
                with client.wrap_socket(raw, server_hostname=expected_name) as secure:
                    result["received"] = secure.recv(32)
        except ssl.SSLError as exc:
            result["client_error"] = exc
        finally:
            thread.join(timeout=6)
            if thread.is_alive():
                raise RuntimeError("Local TLS test did not finish")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pki", type=Path, required=True)
    parser.add_argument("--server-ip", default="10.30.192.176")
    args = parser.parse_args()
    root = args.pki.resolve()
    cert, key = root / "device/client.crt", root / "device/client.key"
    valid = exchange(root, cert, key, args.server_ip)
    assert valid.get("received") == b"verified", valid
    no_cert = exchange(root, None, None, args.server_ip)
    assert isinstance(no_cert.get("server_error"), ssl.SSLError) and "received" not in no_cert, no_cert
    wrong_ip = exchange(root, cert, key, "192.0.2.1")
    assert isinstance(wrong_ip.get("client_error"), ssl.SSLCertVerificationError), wrong_ip
    with tempfile.TemporaryDirectory(prefix="mqtt-untrusted-test-") as tmp:
        path = Path(tmp)
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256",
                        "-days", "1", "-subj", "/CN=untrusted-test-only",
                        "-keyout", str(path / "wrong.key"), "-out", str(path / "wrong.crt")],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        rejected = exchange(root, path / "wrong.crt", path / "wrong.key", args.server_ip)
        assert isinstance(rejected.get("server_error"), ssl.SSLCertVerificationError), rejected
    print("PASS: local TLS 1.2 accepts the intended client, rejects no certificate/untrusted client, and checks server IP SAN")
    print("This is a local certificate test, not a live EMQX verification.")


if __name__ == "__main__":
    main()
