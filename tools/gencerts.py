"""Make test certificates and signatures with openssl.
gencerts.py <dir>: writes them there (flat), and prints what to expect."""
import os, subprocess, sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)


def ssl(*args, stdin=None):
    subprocess.run(["openssl", *args], check=True, cwd=out, input=stdin, capture_output=True)


def path(name):
    return os.path.join(out, name)


def ext(name, text):
    with open(path(name), "w") as f:
        f.write(text)


ext("ca.ext", "basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n")
ext("leaf.ext", "basicConstraints=CA:FALSE\nsubjectAltName=DNS:test.rdx,DNS:*.wild.rdx,IP:10.0.2.2\n")
ext("other.ext", "basicConstraints=CA:FALSE\nsubjectAltName=DNS:elsewhere.example\n")

# An RSA authority (4096-bit) and a site certificate (2048-bit RSA) from it.
ssl("req", "-x509", "-newkey", "rsa:4096", "-nodes", "-keyout", "ca_rsa.key", "-out", "ca_rsa.pem", "-days", "3650",
    "-subj", "/O=RDX Test RSA Authority/CN=RDX Test RSA Root", "-addext", "basicConstraints=critical,CA:TRUE",
    "-addext", "keyUsage=critical,keyCertSign,cRLSign")
ssl("req", "-newkey", "rsa:2048", "-nodes", "-keyout", "leaf_rsa.key", "-out", "leaf_rsa.csr", "-subj", "/CN=test.rdx")
ssl("x509", "-req", "-in", "leaf_rsa.csr", "-CA", "ca_rsa.pem", "-CAkey", "ca_rsa.key", "-CAcreateserial", "-out",
    "leaf_rsa.pem", "-days", "365", "-sha256", "-extfile", "leaf.ext")

# An EC authority (P-384) with an intermediate (P-384) and a site certificate (P-256).
ssl("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-384", "-nodes", "-keyout", "ca_ec.key", "-out",
    "ca_ec.pem", "-days", "3650", "-sha384", "-subj", "/O=RDX Test EC Authority/CN=RDX Test EC Root",
    "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign")
ssl("req", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-384", "-nodes", "-keyout", "mid_ec.key", "-out", "mid_ec.csr",
    "-subj", "/O=RDX Test Intermediate/CN=RDX Test EC Intermediate")
ssl("x509", "-req", "-in", "mid_ec.csr", "-CA", "ca_ec.pem", "-CAkey", "ca_ec.key", "-CAcreateserial", "-out", "mid_ec.pem",
    "-days", "1000", "-sha384", "-extfile", "ca.ext")
ssl("req", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-keyout", "leaf_ec.key", "-out", "leaf_ec.csr",
    "-subj", "/CN=test.rdx")
ssl("x509", "-req", "-in", "leaf_ec.csr", "-CA", "mid_ec.pem", "-CAkey", "mid_ec.key", "-CAcreateserial", "-out", "leaf_ec.pem",
    "-days", "365", "-sha256", "-extfile", "leaf.ext")
# A certificate for another name, and one nobody trusted signed.
ssl("x509", "-req", "-in", "leaf_rsa.csr", "-CA", "ca_rsa.pem", "-CAkey", "ca_rsa.key", "-CAcreateserial", "-out",
    "other.pem", "-days", "365", "-sha256", "-extfile", "other.ext")
ssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "self.key", "-out", "self.pem", "-days", "30",
    "-subj", "/CN=test.rdx", "-addext", "subjectAltName=DNS:test.rdx,IP:10.0.2.2")

# Chains as a server would send them (site first).
def cat(dst, *srcs):
    with open(path(dst), "w", newline="\n") as f:
        for s in srcs:
            f.write(open(path(s)).read())


cat("chain_rsa.pem", "leaf_rsa.pem")
cat("chain_ec.pem", "leaf_ec.pem", "mid_ec.pem")
cat("chain_ec_nomid.pem", "leaf_ec.pem")
cat("test_roots.pem", "ca_rsa.pem", "ca_ec.pem")

# Signatures over some data.
with open(path("sigdata.bin"), "wb") as f:
    f.write(b"The quick brown fox signs over the lazy dog. " * 20)
ssl("dgst", "-sha256", "-sign", "leaf_rsa.key", "-out", "sig_pkcs1.bin", "sigdata.bin")
ssl("dgst", "-sha256", "-sign", "leaf_rsa.key", "-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:digest",
    "-out", "sig_pss256.bin", "sigdata.bin")
ssl("dgst", "-sha384", "-sign", "leaf_rsa.key", "-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:digest",
    "-out", "sig_pss384.bin", "sigdata.bin")
ssl("dgst", "-sha256", "-sign", "leaf_ec.key", "-out", "sig_ec256.bin", "sigdata.bin")
ssl("dgst", "-sha384", "-sign", "ca_ec.key", "-out", "sig_ec384.bin", "sigdata.bin")
# A tampered copy of the data.
data = bytearray(open(path("sigdata.bin"), "rb").read())
data[5] ^= 1
open(path("sigdata2.bin"), "wb").write(bytes(data))
for junk in ["ca.ext", "leaf.ext", "other.ext", "leaf_rsa.csr", "mid_ec.csr", "leaf_ec.csr", "ca_rsa.srl", "ca_ec.srl", "mid_ec.srl"]:
    try:
        os.remove(path(junk))
    except OSError:
        pass
import hashlib
print("sha384", hashlib.sha384(open(path("sigdata.bin"), "rb").read()).hexdigest())
print("sha512", hashlib.sha512(open(path("sigdata.bin"), "rb").read()).hexdigest())
