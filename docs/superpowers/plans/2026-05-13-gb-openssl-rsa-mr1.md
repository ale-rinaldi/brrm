# gb.openssl RSA — MR #1 (PKey + Signature) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Contribute MR #1 to Gambas upstream (gitlab.com/gambas/gambas) implementing the RSA `PKey` and `Signature` classes in the `gb.openssl` component, completing the `CSignature` placeholder reserved by Tobias Boege in 2013.

**Architecture:** Three new C files (`c_pkey.{c,h}`, `c_signature.{c,h}`) wired into `gb.openssl/src/main.c` and `Makefile.am`. Two new Gambas classes: `PKey` (instance class wrapping `EVP_PKEY *`, `GB_NOT_CREATABLE`, with factory statics) and `Signature` (factory + `.Signature.Method` virtual class mirroring the existing `Digest`/`Cipher` pattern). Signature algorithms encoded via lookup table on a method string (`"rsa-pkcs1-sha256"`, `"rsa-pss-sha256"`, etc.) consistent with `Cipher["aes-256-cbc"]`.

**Tech Stack:** C (against libcrypto / OpenSSL 1.1+ EVP API), Gambas component build system (autotools — `reconf-all`, `configure`, `make`), Gambas test framework (`gb.test`) inside the component-specific `openssl-test/` project.

**Companion spec:** `docs/superpowers/specs/2026-05-12-gb-openssl-rsa-design.md` — read it first for context.

---

## File Structure

| File | Responsibility | Op |
|---|---|---|
| `gb.openssl/src/c_pkey.h` | Public declaration `extern GB_DESC CPKey[]` + `CPKEY` struct typedef. | Create |
| `gb.openssl/src/c_pkey.c` | `PKey` class implementation: struct, `_free`, factory statics, properties, import/export methods, lookup helpers. | Create |
| `gb.openssl/src/c_signature.h` | Public declaration `extern GB_DESC CSignature[]`, `CSignatureMethod[]`. | Create |
| `gb.openssl/src/c_signature.c` | `Signature` + `.Signature.Method` implementation, algorithm lookup table, Sign/Verify logic. | Create |
| `gb.openssl/src/main.c` | Decomment `CSignature`/`CSignatureMethod` entries, add `CPKey` entry, include new headers. | Modify |
| `gb.openssl/src/Makefile.am` | Add the four new files to `gb_openssl_la_SOURCES`. | Modify |
| `gb.openssl/openssl-test/.src/Main.module` | Add `Test.*` assertions for PKey + Signature. | Modify |

Out of scope for MR #1: `c_pkeycipher.{c,h}` (separate MR #2 — see spec).

---

## Phase A — Setup

### Task 1: Fork upstream, clone, branch, verify baseline build

**Files:** none modified in-repo — workspace setup only.

- [ ] **Step 1: Fork the upstream repository to the `porech` organization on GitLab**

Open https://gitlab.com/gambas/gambas in a browser, click "Fork", select organization `porech`. The user's preference: forks of upstream projects go to the `porech` namespace.

The fork URL will be `https://gitlab.com/porech/gambas`.

- [ ] **Step 2: Clone the fork locally**

```bash
cd ~/src                                  # or wherever you keep checkouts
git clone git@gitlab.com:porech/gambas.git
cd gambas
git remote add upstream https://gitlab.com/gambas/gambas.git
git fetch upstream
```

- [ ] **Step 3: Install build dependencies (Debian/Ubuntu/Mint reference list)**

```bash
sudo apt-get install build-essential autoconf automake libtool pkg-config \
                     libssl-dev libgmp-dev libsqlite3-dev libcurl4-openssl-dev
```

On other distros, install the equivalents; you specifically need `openssl-dev`/`libssl-dev` for our work. The Gambas wiki (https://gambaswiki.org/wiki/install) lists the full set.

- [ ] **Step 4: Bootstrap autotools (whole tree)**

```bash
./reconf-all
```

Run from the repo root. This invokes `libtoolize` recursively + `autoreconf -v --install`. Required only once, and again any time a `Makefile.am` or `configure.ac` is edited.

Expected: ends without "error:" lines. Warnings are normal.

- [ ] **Step 5: Configure**

```bash
./configure -C
```

`-C` caches results in `config.cache`, speeding up future reconfigs.

Expected at the end: a summary listing enabled components — `gb.openssl` must appear under "components". If it doesn't, your `libssl-dev` install is missing/broken.

- [ ] **Step 6: Build the whole tree**

```bash
make -j $(nproc)
```

Takes several minutes the first time. Builds `gbi3`/`gbs3`/`gbc3`/`gbx3` and all enabled components.

Expected: ends with no "Error" lines.

- [ ] **Step 7: Install**

```bash
sudo make install
```

Installs to `/usr/local/`. Gambas resolves components at runtime from `/usr/local/lib/gambas3/`, not from the build tree — this step is required for the test project to find `gb.openssl`.

- [ ] **Step 8: Verify the test project runs on the baseline (pre-changes)**

```bash
cd gb.openssl/openssl-test
gbs3 -V .
```

`gbs3 -V .` compiles and executes the Gambas project in this directory.

Expected: a `Test.Plan(N)` output with all assertions passing (PBKDF2, scrypt, etc.). If this fails on master, fix the environment before continuing — your patches won't be reviewable.

- [ ] **Step 9: Create the feature branch**

```bash
cd ../..                                  # back to repo root
git checkout -b feature/gb-openssl-rsa-signature
```

- [ ] **Step 10: Confirm git author email is correct for upstream contribution**

```bash
git config user.email
```

Expected: `ale@alerinaldi.it` (per user preference for this kind of upstream contribution — see memory `feedback_git_email.md`). If different, set it locally:

```bash
git config user.email ale@alerinaldi.it
```

---

## Phase B — PKey class

### Task 2: Add empty PKey class scaffolding

**Files:**
- Create: `gb.openssl/src/c_pkey.h`
- Create: `gb.openssl/src/c_pkey.c`
- Modify: `gb.openssl/src/main.c`
- Modify: `gb.openssl/src/Makefile.am`

No Gambas-side test in this task — we're just getting compilation green with a class that declares itself but has no behavior beyond `_free`.

- [ ] **Step 1: Create `c_pkey.h`**

Style rules (verified against gb.openssl headers):
- Header description in the comment block is just the filename — no " - description" suffix (`.h` files omit the suffix; `.c` files include it).
- `.h` files include no OpenSSL headers and no `main.h`. Consumers include the right things in the correct order before including the .h.
- The `#define THIS` macro lives in the `.c` file, not the header (avoids leaking the macro to any other file that includes this header — e.g., the future `c_signature.c`).
- Indentation is **hard tabs** (8-col equivalent), K&R braces, `else` on the same line as `}`.

```c
/*
 * c_pkey.h
 *
 * Copyright (C) 2026 Alessandro Rinaldi
 * Copyright (C) 2013-2019 Tobias Boege <tobias@gambas-buch.de>
 *
 * [GPL v2+ with OpenSSL linking exception — COPY VERBATIM the ~33-line
 *  block from gb.openssl/src/c_cipher.h, only updating the copyright lines
 *  above]
 */

#ifndef __C_PKEY_H
#define __C_PKEY_H

#ifndef __C_PKEY_C
extern GB_DESC CPKey[];
#endif

/* Forward declaration so this header can mention EVP_PKEY without
 * pulling in <openssl/evp.h>. Consumers that need the full type
 * must include <openssl/evp.h> themselves before c_pkey.h. */
typedef struct evp_pkey_st EVP_PKEY;

typedef struct {
	GB_BASE   ob;
	EVP_PKEY *pkey;
} CPKEY;

#endif /* __C_PKEY_H */
```

The license header pattern is critical — copy the full GPL+OpenSSL-exception block from `c_cipher.h` (about 33 lines), only swapping the copyright owner lines. Benoît rejects MRs that don't preserve this.

- [ ] **Step 2: Create `c_pkey.c` with just `_free`**

```c
/*
 * c_pkey.c - asymmetric key class implementation
 *
 * [Same full GPL+OpenSSL-exception header as c_pkey.h — note that .c
 *  files include the " - description" suffix in the first comment line]
 */

#define __C_PKEY_C

#include <openssl/evp.h>

#include "main.h"
#include "c_pkey.h"

#define THIS ((CPKEY *) _object)

BEGIN_METHOD_VOID(PKey_free)

	if (THIS->pkey) {
		EVP_PKEY_free(THIS->pkey);
		THIS->pkey = NULL;
	}

END_METHOD

GB_DESC CPKey[] = {
	GB_DECLARE("PKey", sizeof(CPKEY)),
	GB_NOT_CREATABLE(),

	GB_METHOD("_free", NULL, PKey_free, NULL),

	GB_END_DECLARE
};
```

Include order matters: `<openssl/evp.h>` (provides the real `EVP_PKEY`) must come before `main.h` (provides `GB_BASE`, `GB_DESC`, etc.) which must come before `c_pkey.h` (defines `CPKEY` using both types). The forward declaration in `c_pkey.h` is identical to the real OpenSSL typedef so duplicate-typedef rules of C11+ accept both being present.

- [ ] **Step 3: Wire into `main.c`**

Open `gb.openssl/src/main.c`. Add include after the existing `#include "c_hmac.h"`:

```c
#include "c_pkey.h"
```

In the `GB_CLASSES[]` array, add `CPKey,` between `CHMac,` and the commented `CSignature,` line:

```c
GB_DESC *GB_CLASSES[] EXPORT = {
    COpenSSL,
    CDigest, CDigestMethod,
    CCipher, CCipherMethod, CCipherText,
    CHMac,
    CPKey,                                     /* ← added */
//  CSignature,
//  CSignatureMethod,
    NULL
};
```

- [ ] **Step 4: Add the new files to `Makefile.am`**

Open `gb.openssl/src/Makefile.am`. Find the `gb_openssl_la_SOURCES = ...` list. Add the two new source files:

```makefile
gb_openssl_la_SOURCES = \
    main.c main.h \
    c_openssl.c c_openssl.h \
    c_digest.c c_digest.h \
    c_cipher.c c_cipher.h \
    c_hmac.c c_hmac.h \
    c_pkey.c c_pkey.h
```

(Match whatever exact formatting style the existing list uses — backslash-newline continuation is standard autotools.)

- [ ] **Step 5: Re-bootstrap and rebuild (Makefile.am changed)**

```bash
./reconf-all
./configure -C
cd gb.openssl
make -j $(nproc)
```

Expected: `gb.openssl.la` rebuilds, ending with `Wrote: gb.openssl.la` or equivalent linker success line. Errors here are typically:
- Missing `.h` include
- Forgot to add to Makefile.am
- Typo in `GB_DESC` macro

- [ ] **Step 6: Install and verify the class is visible**

```bash
sudo make install
cd openssl-test
```

Add a smoke check at the top of `.src/Main.module`'s `Main()` (temporarily — we'll remove this after, it's just a sanity check):

```gambas
Print "PKey class loaded: "; (Object.Class("PKey") <> Null)
```

Run:

```bash
gbs3 -V .
```

Expected output includes:

```
PKey class loaded: True
```

Then **revert** the temporary `Print` line (we add real tests in later tasks).

- [ ] **Step 7: Commit**

```bash
cd ../..                                  # back to repo root
git add gb.openssl/src/c_pkey.h gb.openssl/src/c_pkey.c \
        gb.openssl/src/main.c gb.openssl/src/Makefile.am
git commit -m "WIP: gb.openssl: scaffold PKey class"
```

This is a WIP commit — at the end we'll squash the whole branch into one logical commit per the upstream commit message format (see Task 21). For now, frequent commits make the branch easier to navigate and to revert specific changes during development.

---

### Task 3: PKey.Generate(Bits) — first working method

**Files:**
- Modify: `gb.openssl/src/c_pkey.c`
- Modify: `gb.openssl/openssl-test/.src/Main.module`

- [ ] **Step 1: Add the failing test in `Main.module`**

Open `gb.openssl/openssl-test/.src/Main.module`. At the bottom of the file, add a new test subroutine:

```gambas
Public Sub Test_PKey_Generate_Returns2048BitKey()
  Dim k As PKey
  k = PKey.Generate(2048)
  Assert.NotNull(k, "Generate returns an object")
  ' We'll add Type/Bits/IsPrivate assertions in Task 4 after properties exist.
End
```

In the `Main()` Sub, locate the existing `Test.Plan(N)` call. Increment N by 1, and add a line to call the new test:

```gambas
Public Sub Main()
  Test.Plan(<current_N + 1>)
  ' ... existing test calls ...
  Test_PKey_Generate_Returns2048BitKey()
End
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd gb.openssl/openssl-test
gbs3 -V .
```

Expected: error like `Unknown identifier: PKey.Generate` or `No such method`.

- [ ] **Step 3: Implement `PKey.Generate` in `c_pkey.c`**

Add this `BEGIN_METHOD` before the `GB_DESC` table:

```c
/**G
 * Generate a new RSA key pair of the given modulus size in bits.
 * Reasonable values are 2048 (default for most uses), 3072, or 4096.
 * The public exponent is fixed to 65537. Returns the new key.
 *
 * This operation is computationally expensive (seconds, sometimes more
 * for 4096 bits) and consumes entropy from the OS.
 **/
BEGIN_METHOD(PKey_Generate, GB_INTEGER bits)

    EVP_PKEY_CTX *ctx = NULL;
    EVP_PKEY *pkey = NULL;
    CPKEY *res;

    if (VARG(bits) < 512) {
        GB.Error("Invalid argument: Bits must be at least 512");
        return;
    }

    ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!ctx) {
        MAIN_error("RSA keygen failed: &1");
        return;
    }
    if (EVP_PKEY_keygen_init(ctx) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        MAIN_error("RSA keygen init failed: &1");
        return;
    }
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, VARG(bits)) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        MAIN_error("RSA keygen bits set failed: &1");
        return;
    }
    if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        MAIN_error("RSA keygen failed: &1");
        return;
    }
    EVP_PKEY_CTX_free(ctx);

    res = (CPKEY *) GB.New(GB.FindClass("PKey"), NULL, NULL);
    if (!res) {
        EVP_PKEY_free(pkey);
        return;  /* GB.New already raised the error */
    }
    res->pkey = pkey;
    GB.ReturnObject(res);

END_METHOD
```

Add the entry to the `GB_DESC` table (between `GB_NOT_CREATABLE()` and `GB_METHOD("_free", ...)`):

```c
    GB_STATIC_METHOD("Generate", "PKey", PKey_Generate, "(Bits)i"),
```

**Implementation note from spec § "Nota implementativa"**: `GB.New(class, NULL, NULL)` on a `GB_NOT_CREATABLE` class needs verification — this is the first place we touch it. If `GB.New` returns NULL and raises an error, the class needs a `_new` method (even a no-op). The fallback fix:

```c
BEGIN_METHOD_VOID(PKey_new)
END_METHOD

/* and in GB_DESC: */
    GB_METHOD("_new", NULL, PKey_new, NULL),
```

- [ ] **Step 4: Rebuild, install, run**

```bash
cd ..                                     # to gb.openssl/
make && sudo make install
cd openssl-test
gbs3 -V .
```

Expected: the new test passes ("1..N ok"). If `GB.New` triggered the issue above, add the no-op `_new` and retry.

- [ ] **Step 5: Commit**

```bash
cd ../..
git add gb.openssl/src/c_pkey.c gb.openssl/openssl-test/.src/Main.module
git commit -m "WIP: PKey.Generate(Bits)"
```

---

### Task 4: PKey properties (Type, Bits, IsPrivate)

**Files:**
- Modify: `gb.openssl/src/c_pkey.c`
- Modify: `gb.openssl/openssl-test/.src/Main.module`

- [ ] **Step 1: Strengthen the test from Task 3**

Update `Test_PKey_Generate_Returns2048BitKey`:

```gambas
Public Sub Test_PKey_Generate_Returns2048BitKey()
  Dim k As PKey
  k = PKey.Generate(2048)
  Assert.NotNull(k, "Generate returns an object")
  Assert.Equals(k.Type, "RSA", "Generated key has Type RSA")
  Assert.Equals(k.Bits, 2048, "Generated key has 2048 bits")
  Assert.True(k.IsPrivate, "Generated key has private material")
End
```

The `Test.Plan(N)` total must be increased by 3 additional assertions (or use Assert.* which auto-counts — check the existing tests for the local convention).

- [ ] **Step 2: Run test, see properties fail**

```bash
cd gb.openssl/openssl-test
gbs3 -V .
```

Expected: error on `k.Type` (`Unknown identifier`).

- [ ] **Step 3: Implement the three properties in `c_pkey.c`**

```c
/**G
 * Return the algorithm family of this key: "RSA" for RSA keys.
 * Future versions may return "EC", "ED25519", etc. for other algorithms.
 **/
BEGIN_PROPERTY(PKey_Type)
    int t = EVP_PKEY_base_id(THIS->pkey);
    const char *name;
    switch (t) {
        case EVP_PKEY_RSA:   name = "RSA"; break;
        default:             name = OBJ_nid2sn(t); break;
    }
    GB.ReturnNewZeroString(name);
END_PROPERTY

/**G
 * Return the key size in bits (e.g. 2048 for an RSA-2048 key, meaning
 * a 2048-bit modulus).
 **/
BEGIN_PROPERTY(PKey_Bits)
    GB.ReturnInteger(EVP_PKEY_bits(THIS->pkey));
END_PROPERTY

/**G
 * Return TRUE if this key contains private material (and can therefore
 * be used to Sign or Decrypt). Return FALSE if it is a public-only key
 * (which can only Verify or Encrypt).
 **/
BEGIN_PROPERTY(PKey_IsPrivate)
    /* In OpenSSL, an EVP_PKEY built from a private PEM has private params;
     * one built from a public PEM does not. We check by trying to access
     * the private key components. The simplest cross-version check is to
     * round-trip through i2d_PrivateKey: if it succeeds, we have private. */
    int len = i2d_PrivateKey(THIS->pkey, NULL);
    GB.ReturnBoolean(len > 0);
END_PROPERTY
```

Note on `i2d_PrivateKey`: in older OpenSSL versions it may return -1 with an error queue set on failure. Clear the error queue afterwards to avoid polluting unrelated calls:

```c
    if (len <= 0) ERR_clear_error();
    GB.ReturnBoolean(len > 0);
```

Add the descriptor entries (between `GB_NOT_CREATABLE()` and the static `Generate` method, or wherever properties go conventionally):

```c
    GB_PROPERTY_READ("Type",      "s", PKey_Type),
    GB_PROPERTY_READ("Bits",      "i", PKey_Bits),
    GB_PROPERTY_READ("IsPrivate", "b", PKey_IsPrivate),
```

- [ ] **Step 4: Rebuild, install, run**

```bash
cd ..
make && sudo make install
cd openssl-test
gbs3 -V .
```

Expected: all three assertions pass.

- [ ] **Step 5: Commit**

```bash
cd ../..
git add gb.openssl/src/c_pkey.c gb.openssl/openssl-test/.src/Main.module
git commit -m "WIP: PKey.Type/Bits/IsPrivate"
```

---

### Task 5: PKey.LoadPrivate (no passphrase) + PKey.ToPem (no passphrase)

These two are best done together because they enable a round-trip test, which is the cheapest way to validate both.

**Files:**
- Modify: `gb.openssl/src/c_pkey.c`
- Modify: `gb.openssl/openssl-test/.src/Main.module`

- [ ] **Step 1: Add the failing round-trip test**

In `Main.module`:

```gambas
Public Sub Test_PKey_PemRoundTrip()
  Dim k1, k2 As PKey
  Dim pem1, pem2 As String

  k1 = PKey.Generate(2048)
  pem1 = k1.ToPem()
  Assert.True(InStr(pem1, "-----BEGIN") > 0, "ToPem produces a PEM blob")

  k2 = PKey.LoadPrivate(pem1)
  pem2 = k2.ToPem()
  Assert.Equals(pem2, pem1, "Round-trip PEM is stable")
  Assert.Equals(k2.Bits, 2048, "Loaded key has same bit length")
End
```

- [ ] **Step 2: Run test, see failure**

```bash
cd gb.openssl/openssl-test && gbs3 -V .
```

Expected: error on `.ToPem` or `.LoadPrivate`.

- [ ] **Step 3: Implement both in `c_pkey.c`**

```c
/**G
 * Load an RSA private key from a PEM-encoded string. If the PEM is
 * encrypted, Passphrase must be supplied; otherwise it can be omitted
 * or passed as an empty string.
 *
 * Raises an exception if the PEM is malformed, if the passphrase is
 * wrong, or if no private material can be parsed from the input.
 **/
BEGIN_METHOD(PKey_LoadPrivate, GB_STRING pem; GB_STRING passphrase)

    BIO *bio = NULL;
    EVP_PKEY *pkey = NULL;
    char *pwd = NULL;
    CPKEY *res;

    bio = BIO_new_mem_buf(STRING(pem), LENGTH(pem));
    if (!bio) {
        MAIN_error("Cannot create BIO: &1");
        return;
    }
    if (!MISSING(passphrase) && LENGTH(passphrase) > 0)
        pwd = GB.ToZeroString(ARG(passphrase));

    pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, pwd);
    BIO_free(bio);
    if (!pkey) {
        MAIN_error("Cannot parse private key: &1");
        return;
    }

    res = (CPKEY *) GB.New(GB.FindClass("PKey"), NULL, NULL);
    res->pkey = pkey;
    GB.ReturnObject(res);

END_METHOD

/**G
 * Export this key as a PEM-encoded string.
 * If this is a private key and Passphrase is supplied, the PEM will be
 * encrypted with AES-256-CBC. Without passphrase, an unencrypted PEM is
 * returned. For public keys, Passphrase is ignored.
 **/
BEGIN_METHOD(PKey_ToPem, GB_STRING passphrase)

    BIO *bio = NULL;
    char *data;
    long len;
    int ok;

    bio = BIO_new(BIO_s_mem());
    if (!bio) { MAIN_error("Cannot create BIO: &1"); return; }

    /* If private, use PEM_write_bio_PrivateKey (with optional encryption);
     * if public, use PEM_write_bio_PUBKEY. We dispatch on IsPrivate. */
    if (i2d_PrivateKey(THIS->pkey, NULL) > 0) {
        if (!MISSING(passphrase) && LENGTH(passphrase) > 0) {
            ok = PEM_write_bio_PrivateKey(bio, THIS->pkey,
                                          EVP_aes_256_cbc(),
                                          (unsigned char *) STRING(passphrase),
                                          LENGTH(passphrase),
                                          NULL, NULL);
        } else {
            ok = PEM_write_bio_PrivateKey(bio, THIS->pkey,
                                          NULL, NULL, 0, NULL, NULL);
        }
    } else {
        ERR_clear_error();        /* i2d_PrivateKey set the error queue */
        ok = PEM_write_bio_PUBKEY(bio, THIS->pkey);
    }

    if (!ok) {
        BIO_free(bio);
        MAIN_error("PEM export failed: &1");
        return;
    }

    len = BIO_get_mem_data(bio, &data);
    GB.ReturnNewString(data, len);
    BIO_free(bio);

END_METHOD
```

Add to `GB_DESC`:

```c
    GB_STATIC_METHOD("LoadPrivate", "PKey", PKey_LoadPrivate, "(Pem)s[(Passphrase)s]"),
    GB_METHOD("ToPem", "s", PKey_ToPem, "[(Passphrase)s]"),
```

Add the needed includes at the top of `c_pkey.c` (after the existing `#include "c_pkey.h"`):

```c
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/objects.h>
```

- [ ] **Step 4: Rebuild, install, run**

```bash
cd .. && make && sudo make install && cd openssl-test && gbs3 -V .
```

Expected: both new assertions pass.

- [ ] **Step 5: Commit**

```bash
cd ../..
git add gb.openssl/src/c_pkey.c gb.openssl/openssl-test/.src/Main.module
git commit -m "WIP: PKey.LoadPrivate + PKey.ToPem (no passphrase)"
```

---

### Task 6: PKey encrypted PEM (with passphrase)

**Files:**
- Modify: `gb.openssl/openssl-test/.src/Main.module` only — implementation already supports it.

- [ ] **Step 1: Add the encrypted round-trip test**

```gambas
Public Sub Test_PKey_EncryptedPemRoundTrip()
  Dim k1, k2 As PKey
  Dim pem As String
  Dim passphrase As String = "correct horse battery staple"

  k1 = PKey.Generate(2048)
  pem = k1.ToPem(passphrase)
  Assert.True(InStr(pem, "ENCRYPTED") > 0, "Encrypted PEM contains ENCRYPTED marker")

  k2 = PKey.LoadPrivate(pem, passphrase)
  Assert.Equals(k2.Bits, 2048, "Loaded encrypted key has 2048 bits")
End
```

- [ ] **Step 2: Add a test for wrong-passphrase error**

```gambas
Public Sub Test_PKey_LoadPrivate_WrongPassphraseRaisesError()
  Dim k As PKey = PKey.Generate(2048)
  Dim pem As String = k.ToPem("right")
  Try k = PKey.LoadPrivate(pem, "wrong")
  Assert.True(Error, "Loading with wrong passphrase raises an error")
End
```

- [ ] **Step 3: Run, expect pass**

```bash
cd gb.openssl/openssl-test && gbs3 -V .
```

Expected: both new assertions pass.

- [ ] **Step 4: Commit**

```bash
cd ../..
git add gb.openssl/openssl-test/.src/Main.module
git commit -m "WIP: tests for PKey encrypted PEM round-trip"
```

---

### Task 7: PKey.LoadPublic + PKey.ToPublicPem

**Files:**
- Modify: `gb.openssl/src/c_pkey.c`
- Modify: `gb.openssl/openssl-test/.src/Main.module`

- [ ] **Step 1: Add the failing test**

```gambas
Public Sub Test_PKey_PublicPemRoundTrip()
  Dim k As PKey = PKey.Generate(2048)
  Dim pubPem As String = k.ToPublicPem()
  Assert.True(InStr(pubPem, "BEGIN PUBLIC KEY") > 0, "ToPublicPem produces SPKI PEM")

  Dim k2 As PKey = PKey.LoadPublic(pubPem)
  Assert.Equals(k2.Bits, 2048, "Loaded public key has correct bits")
  Assert.False(k2.IsPrivate, "Loaded public key is not private")
End
```

- [ ] **Step 2: Run, see failure**

```bash
cd gb.openssl/openssl-test && gbs3 -V .
```

- [ ] **Step 3: Implement both methods in `c_pkey.c`**

```c
/**G
 * Load an RSA public key from a PEM-encoded string. Accepts SPKI
 * (-----BEGIN PUBLIC KEY-----) format.
 *
 * Raises an exception if the PEM is malformed or contains no public key.
 **/
BEGIN_METHOD(PKey_LoadPublic, GB_STRING pem)

    BIO *bio;
    EVP_PKEY *pkey;
    CPKEY *res;

    bio = BIO_new_mem_buf(STRING(pem), LENGTH(pem));
    if (!bio) { MAIN_error("Cannot create BIO: &1"); return; }

    pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!pkey) { MAIN_error("Cannot parse public key: &1"); return; }

    res = (CPKEY *) GB.New(GB.FindClass("PKey"), NULL, NULL);
    res->pkey = pkey;
    GB.ReturnObject(res);

END_METHOD

/**G
 * Export the public portion of this key as a PEM-encoded string
 * (SPKI / -----BEGIN PUBLIC KEY----- format). Works on both private
 * and public-only keys.
 **/
BEGIN_METHOD_VOID(PKey_ToPublicPem)

    BIO *bio = BIO_new(BIO_s_mem());
    char *data;
    long len;
    if (!bio) { MAIN_error("Cannot create BIO: &1"); return; }

    if (!PEM_write_bio_PUBKEY(bio, THIS->pkey)) {
        BIO_free(bio);
        MAIN_error("Public PEM export failed: &1");
        return;
    }
    len = BIO_get_mem_data(bio, &data);
    GB.ReturnNewString(data, len);
    BIO_free(bio);

END_METHOD
```

Add to `GB_DESC`:

```c
    GB_STATIC_METHOD("LoadPublic", "PKey", PKey_LoadPublic, "(Pem)s"),
    GB_METHOD("ToPublicPem", "s", PKey_ToPublicPem, NULL),
```

- [ ] **Step 4: Rebuild, install, run**

```bash
cd .. && make && sudo make install && cd openssl-test && gbs3 -V .
```

- [ ] **Step 5: Commit**

```bash
cd ../..
git add gb.openssl/src/c_pkey.c gb.openssl/openssl-test/.src/Main.module
git commit -m "WIP: PKey.LoadPublic + PKey.ToPublicPem"
```

---

### Task 8: PKey.PublicPart property

**Files:**
- Modify: `gb.openssl/src/c_pkey.c`
- Modify: `gb.openssl/openssl-test/.src/Main.module`

- [ ] **Step 1: Add the failing test**

```gambas
Public Sub Test_PKey_PublicPart_ExtractsPublic()
  Dim priv As PKey = PKey.Generate(2048)
  Assert.True(priv.IsPrivate, "Original is private")

  Dim pub As PKey = priv.PublicPart
  Assert.False(pub.IsPrivate, "PublicPart is not private")
  Assert.Equals(pub.Bits, 2048, "PublicPart has same bits")

  ' Same public modulus → same SPKI export
  Assert.Equals(pub.ToPublicPem(), priv.ToPublicPem(), "PublicPart matches original public PEM")
End
```

- [ ] **Step 2: Run, see failure**

```bash
cd gb.openssl/openssl-test && gbs3 -V .
```

- [ ] **Step 3: Implement PublicPart property**

```c
/**G
 * Return a new PKey containing only the public portion of this key.
 * For a public-only key, returns an equivalent public key (effectively
 * a clone). The returned object is independent — freeing it does not
 * affect the original.
 **/
BEGIN_PROPERTY(PKey_PublicPart)

    BIO *bio = BIO_new(BIO_s_mem());
    EVP_PKEY *pub;
    CPKEY *res;

    if (!bio) { MAIN_error("Cannot create BIO: &1"); return; }
    if (!PEM_write_bio_PUBKEY(bio, THIS->pkey)) {
        BIO_free(bio);
        MAIN_error("Cannot extract public part: &1");
        return;
    }
    BIO_seek(bio, 0);     /* reset BIO read pointer to start */
    pub = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!pub) { MAIN_error("Cannot extract public part: &1"); return; }

    res = (CPKEY *) GB.New(GB.FindClass("PKey"), NULL, NULL);
    res->pkey = pub;
    GB.ReturnObject(res);

END_PROPERTY
```

Add to `GB_DESC`:

```c
    GB_PROPERTY_READ("PublicPart", "PKey", PKey_PublicPart),
```

- [ ] **Step 4: Rebuild, install, run**

```bash
cd .. && make && sudo make install && cd openssl-test && gbs3 -V .
```

- [ ] **Step 5: Commit**

```bash
cd ../..
git add gb.openssl/src/c_pkey.c gb.openssl/openssl-test/.src/Main.module
git commit -m "WIP: PKey.PublicPart"
```

---

### Task 9: PKey DER methods (LoadPrivateDer, LoadPublicDer, ToDer, ToPublicDer)

**Files:**
- Modify: `gb.openssl/src/c_pkey.c`
- Modify: `gb.openssl/openssl-test/.src/Main.module`

- [ ] **Step 1: Add failing tests**

```gambas
Public Sub Test_PKey_DerRoundTrip()
  Dim k1 As PKey = PKey.Generate(2048)
  Dim der As String = k1.ToDer()
  Assert.True(Len(der) > 1000, "DER private key is substantial in size")

  Dim k2 As PKey = PKey.LoadPrivateDer(der)
  Assert.True(k2.IsPrivate, "Loaded DER private key is private")
  Assert.Equals(k2.Bits, 2048, "DER round-trip preserves Bits")
  Assert.Equals(k2.ToDer(), der, "DER round-trip is byte-stable")
End

Public Sub Test_PKey_PublicDerRoundTrip()
  Dim k1 As PKey = PKey.Generate(2048).PublicPart
  Dim der As String = k1.ToPublicDer()
  Dim k2 As PKey = PKey.LoadPublicDer(der)
  Assert.False(k2.IsPrivate, "Loaded public DER is not private")
  Assert.Equals(k2.ToPublicDer(), der, "Public DER round-trip is byte-stable")
End
```

- [ ] **Step 2: Run, see failures**

```bash
cd gb.openssl/openssl-test && gbs3 -V .
```

- [ ] **Step 3: Implement the four DER methods in `c_pkey.c`**

```c
/**G
 * Load an RSA private key from a binary DER blob (PKCS#8 format).
 **/
BEGIN_METHOD(PKey_LoadPrivateDer, GB_STRING der)

    const unsigned char *p = (const unsigned char *) STRING(der);
    EVP_PKEY *pkey = d2i_AutoPrivateKey(NULL, &p, LENGTH(der));
    CPKEY *res;

    if (!pkey) { MAIN_error("Cannot parse DER private key: &1"); return; }
    res = (CPKEY *) GB.New(GB.FindClass("PKey"), NULL, NULL);
    res->pkey = pkey;
    GB.ReturnObject(res);

END_METHOD

/**G
 * Load an RSA public key from a binary DER blob (SPKI / X.509
 * SubjectPublicKeyInfo format).
 **/
BEGIN_METHOD(PKey_LoadPublicDer, GB_STRING der)

    const unsigned char *p = (const unsigned char *) STRING(der);
    EVP_PKEY *pkey = d2i_PUBKEY(NULL, &p, LENGTH(der));
    CPKEY *res;

    if (!pkey) { MAIN_error("Cannot parse DER public key: &1"); return; }
    res = (CPKEY *) GB.New(GB.FindClass("PKey"), NULL, NULL);
    res->pkey = pkey;
    GB.ReturnObject(res);

END_METHOD

/**G
 * Export this private key as a binary DER blob (PKCS#8 format).
 * Raises an exception if this is a public-only key.
 **/
BEGIN_METHOD_VOID(PKey_ToDer)

    unsigned char *buf = NULL;
    int len;

    len = i2d_PrivateKey(THIS->pkey, &buf);
    if (len <= 0) {
        ERR_clear_error();
        GB.Error("Key is not a private key");
        return;
    }
    GB.ReturnNewString((char *) buf, len);
    OPENSSL_free(buf);

END_METHOD

/**G
 * Export the public portion as a binary DER blob (SPKI / X.509
 * SubjectPublicKeyInfo format).
 **/
BEGIN_METHOD_VOID(PKey_ToPublicDer)

    unsigned char *buf = NULL;
    int len = i2d_PUBKEY(THIS->pkey, &buf);
    if (len <= 0) { MAIN_error("Public DER export failed: &1"); return; }
    GB.ReturnNewString((char *) buf, len);
    OPENSSL_free(buf);

END_METHOD
```

Add to `GB_DESC`:

```c
    GB_STATIC_METHOD("LoadPrivateDer", "PKey", PKey_LoadPrivateDer, "(Der)s"),
    GB_STATIC_METHOD("LoadPublicDer",  "PKey", PKey_LoadPublicDer,  "(Der)s"),
    GB_METHOD("ToDer",       "s", PKey_ToDer,       NULL),
    GB_METHOD("ToPublicDer", "s", PKey_ToPublicDer, NULL),
```

- [ ] **Step 4: Rebuild, install, run**

```bash
cd .. && make && sudo make install && cd openssl-test && gbs3 -V .
```

- [ ] **Step 5: Commit**

```bash
cd ../..
git add gb.openssl/src/c_pkey.c gb.openssl/openssl-test/.src/Main.module
git commit -m "WIP: PKey DER import/export"
```

---

### Task 10: Add error-path tests for PKey

**Files:**
- Modify: `gb.openssl/openssl-test/.src/Main.module` only.

- [ ] **Step 1: Add failure-mode tests**

```gambas
Public Sub Test_PKey_LoadInvalidPem_RaisesError()
  Dim k As PKey
  Try k = PKey.LoadPrivate("this is not a PEM")
  Assert.True(Error, "Garbage input raises error")
End

Public Sub Test_PKey_GenerateZeroBits_RaisesError()
  Dim k As PKey
  Try k = PKey.Generate(0)
  Assert.True(Error, "Generate(0) raises error")
End

Public Sub Test_PKey_ToDer_OnPublicKey_RaisesError()
  Dim pub As PKey = PKey.Generate(2048).PublicPart
  Dim der As String
  Try der = pub.ToDer()
  Assert.True(Error, "ToDer on public-only key raises error")
End
```

- [ ] **Step 2: Run, expect pass**

```bash
cd gb.openssl/openssl-test && gbs3 -V .
```

All three should already pass — they test error paths we implemented along the way.

- [ ] **Step 3: Commit**

```bash
cd ../..
git add gb.openssl/openssl-test/.src/Main.module
git commit -m "WIP: PKey error-path tests"
```

---

## Phase C — Signature class

### Task 11: Add empty Signature class scaffolding

**Files:**
- Create: `gb.openssl/src/c_signature.h`
- Create: `gb.openssl/src/c_signature.c`
- Modify: `gb.openssl/src/main.c` (decomment + add include)
- Modify: `gb.openssl/src/Makefile.am`

- [ ] **Step 1: Create `c_signature.h`**

Same style rules as `c_pkey.h`: just the filename in the description, no OpenSSL/`main.h` includes, no THIS macro (this class has no instance state — it's a virtual method class, so no THIS needed at all).

```c
/*
 * c_signature.h
 *
 * [Full GPL+OpenSSL-exception header — copy verbatim from c_cipher.h,
 *  with updated copyright lines]
 */

#ifndef __C_SIGNATURE_H
#define __C_SIGNATURE_H

#ifndef __C_SIGNATURE_C
extern GB_DESC CSignature[];
extern GB_DESC CSignatureMethod[];
#endif

#endif /* __C_SIGNATURE_H */
```

- [ ] **Step 2: Create `c_signature.c` with the lookup table + skeleton**

```c
/*
 * c_signature.c - asymmetric signature class implementation
 *
 * [Same full GPL+OpenSSL-exception header as c_signature.h — note the
 *  " - description" suffix in the first comment line for .c files]
 */

#define __C_SIGNATURE_C

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/rsa.h>

#include "main.h"
#include "c_signature.h"
#include "c_pkey.h"

/* CPKEY struct is shared via c_pkey.h. Access pkey->pkey directly
 * here since c_signature.c needs the EVP_PKEY * for sign/verify ops. */

struct sig_algo {
    const char *name;
    int         key_type;
    int         padding;
    const char *md_name;
};

static const struct sig_algo SIG_ALGOS[] = {
    { "rsa-pkcs1-sha256", EVP_PKEY_RSA, RSA_PKCS1_PADDING,     "sha256" },
    { "rsa-pkcs1-sha384", EVP_PKEY_RSA, RSA_PKCS1_PADDING,     "sha384" },
    { "rsa-pkcs1-sha512", EVP_PKEY_RSA, RSA_PKCS1_PADDING,     "sha512" },
    { "rsa-pss-sha256",   EVP_PKEY_RSA, RSA_PKCS1_PSS_PADDING, "sha256" },
    { "rsa-pss-sha384",   EVP_PKEY_RSA, RSA_PKCS1_PSS_PADDING, "sha384" },
    { "rsa-pss-sha512",   EVP_PKEY_RSA, RSA_PKCS1_PSS_PADDING, "sha512" },
    { NULL, 0, 0, NULL }
};

static const struct sig_algo *sig_lookup(const char *name)
{
    int i;
    for (i = 0; SIG_ALGOS[i].name; i++) {
        if (!strcasecmp(SIG_ALGOS[i].name, name))
            return &SIG_ALGOS[i];
    }
    return NULL;
}

/* Per-virtual-object selected algorithm — set by Signature._get */
static const struct sig_algo *_algo;

GB_DESC CSignature[] = {
    GB_DECLARE("Signature", 0),
    GB_NOT_CREATABLE(),
    GB_END_DECLARE
};

GB_DESC CSignatureMethod[] = {
    GB_DECLARE(".Signature.Method", 0),
    GB_VIRTUAL_CLASS(),
    GB_END_DECLARE
};
```

- [ ] **Step 3: Modify `main.c` — decomment placeholders, add include**

In `main.c`, add include after `#include "c_pkey.h"`:

```c
#include "c_signature.h"
```

And **decomment** the existing `CSignature, CSignatureMethod` lines in `GB_CLASSES[]`:

```c
    CPKey,
    CSignature,                                  /* was commented out */
    CSignatureMethod,                            /* was commented out */
    NULL
};
```

- [ ] **Step 4: Modify `Makefile.am` — add the two new files**

```makefile
gb_openssl_la_SOURCES = \
    ... existing ... \
    c_pkey.c c_pkey.h \
    c_signature.c c_signature.h
```

- [ ] **Step 5: Reconfigure + rebuild + install**

```bash
./reconf-all && ./configure -C && cd gb.openssl && make && sudo make install
```

Expected: clean build. The component now exposes `Signature` and `.Signature.Method`, both empty.

- [ ] **Step 6: Smoke check from the test project**

In `Main.module`, temporarily add to `Main()`:

```gambas
Print "Signature loaded: "; (Object.Class("Signature") <> Null)
```

Run `gbs3 -V .`. Expected: `Signature loaded: True`. Then revert the temporary line.

- [ ] **Step 7: Commit**

```bash
cd ../..
git add gb.openssl/src/c_signature.h gb.openssl/src/c_signature.c \
        gb.openssl/src/main.c gb.openssl/src/Makefile.am
git commit -m "WIP: gb.openssl: scaffold Signature class"
```

---

### Task 12: Signature.List, IsSupported, _get + .Signature.Method scaffolding

**Files:**
- Modify: `gb.openssl/src/c_signature.c`
- Modify: `gb.openssl/openssl-test/.src/Main.module`

- [ ] **Step 1: Add the failing test**

```gambas
Public Sub Test_Signature_List_ContainsExpectedMethods()
  Dim names As String[] = Signature.List
  Assert.Contains(names, "rsa-pkcs1-sha256", "List contains rsa-pkcs1-sha256")
  Assert.Contains(names, "rsa-pss-sha256",   "List contains rsa-pss-sha256")
  Assert.True(Signature.IsSupported("rsa-pkcs1-sha256"), "IsSupported true for known")
  Assert.False(Signature.IsSupported("nope-md5"),        "IsSupported false for unknown")
End

Public Sub Test_Signature_UnknownMethod_RaisesError()
  Dim m As Object
  Try m = Signature["nope-md5"]
  Assert.True(Error, "Unknown method raises on indexer")
End
```

`Assert.Contains` may need to be a small helper if `gb.test` doesn't provide it — check first; if absent, use:

```gambas
Public Sub AssertContains(arr As String[], target As String, message As String)
  Dim s As String
  For Each s In arr
    If s = target Then Assert.True(True, message): Return
  Next
  Assert.True(False, message)
End
```

- [ ] **Step 2: Run, see failure**

```bash
cd gb.openssl/openssl-test && gbs3 -V .
```

- [ ] **Step 3: Implement List, IsSupported, _get, plus the empty virtual class**

Update `c_signature.c`. Add forward declarations near the top:

```c
DECLARE_METHOD(Signature_List);
DECLARE_METHOD(Signature_IsSupported);
DECLARE_METHOD(Signature_get);
```

(or just place implementations above the GB_DESC tables — pick the convention used in `c_digest.c`.)

```c
BEGIN_PROPERTY(Signature_List)

    GB_ARRAY arr;
    int i;
    char **slot;

    GB.Array.New(&arr, GB_T_STRING, 0);
    for (i = 0; SIG_ALGOS[i].name; i++) {
        slot = (char **) GB.Array.Add(arr);
        *slot = GB.NewZeroString(SIG_ALGOS[i].name);
    }
    GB.ReturnObject(arr);

END_PROPERTY

BEGIN_METHOD(Signature_IsSupported, GB_STRING method)

    GB.ReturnBoolean(sig_lookup(GB.ToZeroString(ARG(method))) != NULL);

END_METHOD

BEGIN_METHOD(Signature_get, GB_STRING method)

    const struct sig_algo *a = sig_lookup(GB.ToZeroString(ARG(method)));
    if (!a) {
        GB.Error("Unknown signature method");
        return;
    }
    _algo = a;
    RETURN_SELF();

END_METHOD
```

Update `GB_DESC CSignature[]`:

```c
GB_DESC CSignature[] = {
    GB_DECLARE("Signature", 0),
    GB_NOT_CREATABLE(),

    GB_STATIC_PROPERTY_READ("List", "String[]", Signature_List),

    GB_STATIC_METHOD("_get", ".Signature.Method", Signature_get, "(Method)s"),
    GB_STATIC_METHOD("IsSupported", "b", Signature_IsSupported, "(Method)s"),

    GB_END_DECLARE
};
```

- [ ] **Step 4: Rebuild, install, run**

```bash
cd .. && make && sudo make install && cd openssl-test && gbs3 -V .
```

Expected: all 5 new assertions pass.

- [ ] **Step 5: Commit**

```bash
cd ../..
git add gb.openssl/src/c_signature.c gb.openssl/openssl-test/.src/Main.module
git commit -m "WIP: Signature.List + IsSupported + indexer"
```

---

### Task 13: SignatureMethod.Sign + Verify (round-trip)

**Files:**
- Modify: `gb.openssl/src/c_signature.c`
- Modify: `gb.openssl/openssl-test/.src/Main.module`

- [ ] **Step 1: Add the failing test (one round-trip per algorithm)**

```gambas
Public Sub Test_Signature_RoundTrip()
  Dim k As PKey = PKey.Generate(2048)
  Dim data As String = "Hello, gb.openssl!"
  Dim methods As String[] = ["rsa-pkcs1-sha256", "rsa-pkcs1-sha384", "rsa-pkcs1-sha512",
                              "rsa-pss-sha256",   "rsa-pss-sha384",   "rsa-pss-sha512"]
  Dim m As String, sig As String

  For Each m In methods
    sig = Signature[m].Sign(data, k)
    Assert.True(Len(sig) >= 256, "Sig length reasonable for " & m)
    Assert.True(Signature[m].Verify(data, sig, k.PublicPart), "Round-trip Verify true for " & m)
    Assert.False(Signature[m].Verify(data & "x", sig, k.PublicPart), "Tampered data: Verify false for " & m)
  Next
End
```

- [ ] **Step 2: Run, see failure**

```bash
cd gb.openssl/openssl-test && gbs3 -V .
```

- [ ] **Step 3: Implement Sign and Verify**

```c
/**G
 * Sign the given data with the supplied private key using this
 * signature method. Returns the raw binary signature.
 *
 * Raises an exception if Key is not a private key or if OpenSSL
 * rejects the operation.
 **/
BEGIN_METHOD(SignatureMethod_Sign, GB_STRING data; GB_OBJECT key)

    CPKEY *key_obj = (CPKEY *) VARG(key);
    const EVP_MD *md = NULL;
    EVP_MD_CTX *ctx = NULL;
    EVP_PKEY_CTX *pctx = NULL;
    unsigned char *sig = NULL;
    size_t siglen = 0;

    md = EVP_get_digestbyname(_algo->md_name);
    if (!md) { GB.Error("Digest not available"); return; }

    ctx = EVP_MD_CTX_new();
    if (!ctx) { MAIN_error("Cannot create MD context: &1"); return; }

    if (EVP_DigestSignInit(ctx, &pctx, md, NULL, key_obj->pkey) <= 0)
        goto err;

    if (_algo->padding == RSA_PKCS1_PSS_PADDING) {
        if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) <= 0) goto err;
        /* PSS salt length = digest size (default that maps to RSA_PSS_SALTLEN_DIGEST in 1.1+) */
        if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) <= 0) goto err;
    } else {
        if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PADDING) <= 0) goto err;
    }

    if (EVP_DigestSignUpdate(ctx, STRING(data), LENGTH(data)) <= 0) goto err;
    if (EVP_DigestSignFinal(ctx, NULL, &siglen) <= 0) goto err;     /* probe size */
    sig = OPENSSL_malloc(siglen);
    if (!sig) { EVP_MD_CTX_free(ctx); MAIN_error("Out of memory: &1"); return; }
    if (EVP_DigestSignFinal(ctx, sig, &siglen) <= 0) goto err;

    EVP_MD_CTX_free(ctx);
    GB.ReturnNewString((char *) sig, siglen);
    OPENSSL_free(sig);
    return;

err:
    if (sig) OPENSSL_free(sig);
    if (ctx) EVP_MD_CTX_free(ctx);
    MAIN_error("RSA sign failed: &1");

END_METHOD

/**G
 * Verify a signature against the given data using a public (or private)
 * key. The signature should have been produced by Sign() with a matching
 * private key.
 *
 * Returns:
 *   TRUE  - the signature is valid for this data and key
 *   FALSE - the signature does NOT match (data tampered, wrong key, wrong
 *           algorithm, or forged signature)
 *
 * IMPORTANT: a return value of FALSE is NOT an error - the verification
 * completed successfully and concluded that the signature is invalid.
 * The absence of an exception does NOT mean the signature is valid; you
 * MUST check the return value. Using Verify() as if it returned void
 * would silently accept any signature.
 *
 * An exception is raised only if the key is malformed, the algorithm is
 * unavailable, or an internal OpenSSL error occurs.
 **/
BEGIN_METHOD(SignatureMethod_Verify, GB_STRING data; GB_STRING sig; GB_OBJECT key)

    CPKEY *key_obj = (CPKEY *) VARG(key);
    const EVP_MD *md = NULL;
    EVP_MD_CTX *ctx = NULL;
    EVP_PKEY_CTX *pctx = NULL;
    int rc;

    md = EVP_get_digestbyname(_algo->md_name);
    if (!md) { GB.Error("Digest not available"); return; }

    ctx = EVP_MD_CTX_new();
    if (!ctx) { MAIN_error("Cannot create MD context: &1"); return; }

    if (EVP_DigestVerifyInit(ctx, &pctx, md, NULL, key_obj->pkey) <= 0) {
        EVP_MD_CTX_free(ctx);
        MAIN_error("RSA verify init failed: &1");
        return;
    }
    if (_algo->padding == RSA_PKCS1_PSS_PADDING) {
        EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING);
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST);
    } else {
        EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PADDING);
    }

    if (EVP_DigestVerifyUpdate(ctx, STRING(data), LENGTH(data)) <= 0) {
        EVP_MD_CTX_free(ctx);
        MAIN_error("RSA verify update failed: &1");
        return;
    }

    rc = EVP_DigestVerifyFinal(ctx, (unsigned char *) STRING(sig), LENGTH(sig));
    EVP_MD_CTX_free(ctx);

    if (rc == 1) {
        GB.ReturnBoolean(1);
    } else if (rc == 0) {
        ERR_clear_error();      /* signature mismatch — not an error */
        GB.ReturnBoolean(0);
    } else {
        MAIN_error("RSA verify failed: &1");
    }

END_METHOD
```

Update `GB_DESC CSignatureMethod[]`:

```c
GB_DESC CSignatureMethod[] = {
    GB_DECLARE(".Signature.Method", 0),
    GB_VIRTUAL_CLASS(),

    GB_STATIC_METHOD("Sign",   "s", SignatureMethod_Sign,   "(Data)s(Key)PKey"),
    GB_STATIC_METHOD("Verify", "b", SignatureMethod_Verify, "(Data)s(Signature)s(Key)PKey"),

    GB_END_DECLARE
};
```

- [ ] **Step 4: Rebuild, install, run**

```bash
cd .. && make && sudo make install && cd openssl-test && gbs3 -V .
```

Expected: all 6 algorithm round-trips pass.

- [ ] **Step 5: Commit**

```bash
cd ../..
git add gb.openssl/src/c_signature.c gb.openssl/openssl-test/.src/Main.module
git commit -m "WIP: SignatureMethod.Sign + Verify, all 6 RSA variants"
```

---

### Task 14: Wrong-key Verify and unsupported-method tests

**Files:**
- Modify: `gb.openssl/openssl-test/.src/Main.module` only.

- [ ] **Step 1: Add the tests**

```gambas
Public Sub Test_Signature_VerifyWrongKey_ReturnsFalse()
  Dim k1 As PKey = PKey.Generate(2048)
  Dim k2 As PKey = PKey.Generate(2048)
  Dim sig As String = Signature["rsa-pkcs1-sha256"].Sign("hello", k1)
  Assert.False(Signature["rsa-pkcs1-sha256"].Verify("hello", sig, k2.PublicPart), _
               "Verify with wrong public key returns FALSE")
End

Public Sub Test_Signature_VerifyWithPublicKeyForSign_RaisesError()
  Dim pub As PKey = PKey.Generate(2048).PublicPart
  Dim sig As String
  Try sig = Signature["rsa-pkcs1-sha256"].Sign("hello", pub)
  Assert.True(Error, "Signing with public-only key raises error")
End
```

- [ ] **Step 2: Run, expect pass**

```bash
cd gb.openssl/openssl-test && gbs3 -V .
```

- [ ] **Step 3: Commit**

```bash
cd ../..
git add gb.openssl/openssl-test/.src/Main.module
git commit -m "WIP: Signature failure-path tests"
```

---

### Task 15: JWT RS256 known-vector test

**Files:**
- Modify: `gb.openssl/openssl-test/.src/Main.module` only.

This test uses the RFC 7515 Appendix A.2 vector — a published example with a known RSA key, payload, and signature. Verifying it succeeds confirms that our `rsa-pkcs1-sha256` matches the JWT spec exactly.

- [ ] **Step 1: Add the vector + test**

```gambas
' RFC 7515 Appendix A.2 (RS256 signature example).
' Public key (n, e) given in §A.2.1 as JWK; converted to SPKI PEM here.

Private Const RFC7515_A2_PUB_PEM As String = _
  "-----BEGIN PUBLIC KEY-----\n" & _
  "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAofgWCuLjybRlzo0tZWJj\n" & _
  "...[full PEM body from RFC 7515 §A.2.1; see implementation note]...\n" & _
  "-----END PUBLIC KEY-----\n"

Private Const RFC7515_A2_SIGNED_INPUT As String = _
  "eyJhbGciOiJSUzI1NiJ9" & "." & _
  "eyJpc3MiOiJqb2UiLA0KICJleHAiOjEzMDA4MTkzODAsDQogImh0dHA6Ly9leGFtc..."
  ' (full base64url(header) & "." & base64url(payload) from RFC 7515 §A.2)

Private Const RFC7515_A2_SIGNATURE_BASE64URL As String = _
  "cC4hiUPoj9Eetdgtv3hF80EGrhuB__dzERat0XF9g2VtQgr9PJbu3XOiZj5RZmh7..." 
  ' (full base64url signature from RFC 7515 §A.2)

Private Function Base64UrlDecode(s As String) As String
  ' standard base64url → base64 conversion, then decode
  s = Replace(s, "-", "+")
  s = Replace(s, "_", "/")
  While Len(s) Mod 4 <> 0
    s &= "="
  Wend
  Return UnBase64$(s)
End

Public Sub Test_Signature_VerifyKnownVector_Jwt()
  Dim pub As PKey = PKey.LoadPublic(RFC7515_A2_PUB_PEM)
  Dim sig As String = Base64UrlDecode(RFC7515_A2_SIGNATURE_BASE64URL)
  Assert.True(Signature["rsa-pkcs1-sha256"].Verify(RFC7515_A2_SIGNED_INPUT, sig, pub), _
              "RFC 7515 §A.2 (RS256) golden vector verifies")
End
```

**Implementation note**: at implementation time, fetch the actual full PEM and signature from RFC 7515 §A.2 (https://datatracker.ietf.org/doc/html/rfc7515#appendix-A.2). The JWK in the RFC needs conversion from `(n, e)` base64url-decoded big-endian integers into an SPKI PEM. This can be done once with `openssl rsa -in jwk.pem -pubout` after manually building the PEM, or with a small Python script using `cryptography`'s `RSAPublicNumbers`. Save the PEM as a hardcoded constant in the test module.

- [ ] **Step 2: Run, expect pass**

```bash
cd gb.openssl/openssl-test && gbs3 -V .
```

If the test fails, our `rsa-pkcs1-sha256` does NOT match the JWT spec — investigate before continuing.

- [ ] **Step 3: Commit**

```bash
cd ../..
git add gb.openssl/openssl-test/.src/Main.module
git commit -m "WIP: golden-vector test for JWT RS256 (RFC 7515 §A.2)"
```

---

## Phase D — Documentation, polish, MR

### Task 16: Add `/**G */` docstrings to all remaining methods

**Files:**
- Modify: `gb.openssl/src/c_pkey.c` and `gb.openssl/src/c_signature.c`

The docstrings for `Generate`, `Sign`, `Verify` were added inline above. Audit every other method and property: each must have a `/**G ... **/` block immediately preceding its `BEGIN_*`. Also add inline `/**G ... **/` after each entry in the `GB_DESC` table if you want the entry-level summary (see `c_digest.c` for the pattern).

- [ ] **Step 1: Audit every BEGIN_METHOD/BEGIN_PROPERTY in `c_pkey.c` and `c_signature.c`**

Open each file. For each `BEGIN_*` without a preceding `/**G */`, add a 1-4 line description following the style of existing docstrings in `c_digest.c` / `c_cipher.c`.

Specific docstrings to write (templates):

```c
/**G
 * Load an RSA private key from a PEM-encoded string. ...
 **/   /* PKey.LoadPrivate — already added in Task 5 */

/**G
 * Return TRUE if this key contains private material ...
 **/   /* PKey.IsPrivate — already added in Task 4 */

/**G
 * Return a list of all signature methods supported by this build.
 * Currently RSA only: rsa-pkcs1-sha{256,384,512}, rsa-pss-sha{256,384,512}.
 **/   /* Signature.List */

/**G
 * Return a virtual object representing a signature method by giving
 * its name. Valid names can be looked up from Signature.List.
 **/   /* Signature._get */

/**G
 * Check whether the named signature method is supported.
 **/   /* Signature.IsSupported */
```

- [ ] **Step 2: Verify nothing in `_init`/`_exit` machinery is needed**

`c_digest.c` has `_init`/`_exit` for caching its method list. We can skip them — our list is a tiny static table. Confirm no `_init`/`_exit` are referenced in the `GB_DESC` after this task; remove if accidentally added.

- [ ] **Step 3: Rebuild + run tests (sanity check no warnings introduced)**

```bash
cd gb.openssl && make && sudo make install && cd openssl-test && gbs3 -V .
```

- [ ] **Step 4: Commit**

```bash
cd ../..
git add gb.openssl/src/c_pkey.c gb.openssl/src/c_signature.c
git commit -m "WIP: docstrings for all PKey and Signature methods"
```

---

### Task 17: Wiki edit check

**Files:** none in this repo.

- [ ] **Step 1: Check if the Gambas wiki accepts external edits**

Visit https://gambaswiki.org/wiki/comp/gb.openssl/digest in a browser. Look for an "Edit" button or login link. If anonymous/registered edits are possible, plan a wiki MR for after merge (or just edit directly).

- [ ] **Step 2: If wiki edits are possible**

Prepare draft content for two new wiki pages (`gambaswiki.org/wiki/comp/gb.openssl/pkey` and `.../signature`). Use the docstring text as the base, expanded with usage examples. Include the explicit Verify return-value warning in the Signature page. These can be submitted **after** the code MR is merged (the wiki refers to merged API).

- [ ] **Step 3: If wiki edits are NOT possible externally**

In the MR description (Task 19), include an explicit "Wiki update needed" section with the draft content ready for Benoît to paste.

- [ ] **Step 4: Document the decision in the branch (no commit yet — info goes into the MR body)**

Save the decision (wiki MR or wiki update request) for inclusion in the MR description.

---

### Task 18: Squash WIP commits into final commit

**Files:** none modified — git operation only.

Upstream convention is one logical commit per MR with a structured `[GB.OPENSSL]` body. We've been making WIP commits — squash them now.

- [ ] **Step 1: Check the branch history**

```bash
git log --oneline upstream/master..HEAD
```

You should see a list of WIP commits.

- [ ] **Step 2: Interactive rebase to squash**

```bash
git rebase -i upstream/master
```

In the editor, change all but the first line from `pick` to `squash` (or `s`). Save.

In the resulting commit message editor, replace all the WIP messages with the final structured body from the spec:

```
gb.openssl: Add RSA signature support and PKey class

[GB.OPENSSL]
* NEW: PKey: New class for handling asymmetric (RSA) keys, with
  PEM and DER import/export, generation, and introspection.
* NEW: Signature: New class for asymmetric signature algorithms,
  mirroring the Cipher/Digest factory pattern. Supports RSA with
  PKCS#1 v1.5 and PSS padding, and SHA-256/384/512 digests.
* NEW: gb.openssl: This completes the CSignature placeholder that
  has been reserved in main.c since the component was created.
```

Note: do NOT include `Co-Authored-By:` in upstream Gambas commits — that's our convention for our private repo only, and Gambas commits don't use it (verified by inspection of recent master commits).

- [ ] **Step 3: Re-verify build and tests after the squash**

```bash
cd gb.openssl && make && sudo make install && cd openssl-test && gbs3 -V .
```

Expected: all tests still pass. (The squash shouldn't change content, just history.)

- [ ] **Step 4: Confirm the branch is exactly one commit ahead of upstream/master**

```bash
git log --oneline upstream/master..HEAD
```

Expected: exactly one line.

---

### Task 19: Push to the fork and open the MR

**Files:** none modified.

- [ ] **Step 1: Push the branch to the porech fork**

```bash
git push -u origin feature/gb-openssl-rsa-signature
```

The `-u` sets the upstream tracking ref.

- [ ] **Step 2: Open the MR via the GitLab UI or `glab` CLI**

Either:

```bash
glab mr create \
  --target-branch master \
  --target-project gambas/gambas \
  --source-branch feature/gb-openssl-rsa-signature \
  --title "gb.openssl: Add RSA signature support and PKey class" \
  --description-file .git/MR_BODY.md
```

Or paste the body manually in the GitLab web UI.

The MR body content (save as `.git/MR_BODY.md` before invoking `glab`):

```markdown
This MR completes the `CSignature` placeholder reserved in
`gb.openssl/src/main.c` and adds an accompanying `PKey` class to
encapsulate RSA key material.

### New classes
- **PKey** — encapsulates `EVP_PKEY *`. Static factories for loading
  PEM/DER (private with optional passphrase, public) and generating
  new key pairs. Properties: Type, Bits, IsPrivate, PublicPart.
  Instance methods for exporting back to PEM/DER.
- **Signature / .Signature.Method** — factory + virtual class
  following the Digest/Cipher pattern. Currently supports RSA with
  PKCS#1 v1.5 and PSS padding, SHA-256/384/512 digests.

### Design notes
- Algorithm/padding/hash are encoded in the indexer string
  (e.g. `Signature["rsa-pss-sha256"]`) for consistency with the
  existing `Cipher["aes-256-cbc"]` convention.
- Sensible defaults are hardcoded: PSS salt = digest size, MGF1
  matches OAEP hash (relevant in a follow-up MR), exponent = 65537,
  encrypted PEM = AES-256-CBC. Exposing these as parameters can be a
  follow-up MR if needed.
- A follow-up MR will add `PKeyCipher` for RSA public-key encryption
  (PKCS#1 v1.5 / OAEP). Keeping these separate to ease review.

### Tests
Adds 14+ self-tests to `gb.openssl/openssl-test/.src/Main.module`,
including:
- Round-trip PEM/DER import/export, with and without passphrase
- Sign/Verify round-trip for all 6 RSA signature variants
- Known-vector verification from RFC 7515 Appendix A.2 (JWT RS256)
- Tampered-data and wrong-key verification (must return FALSE, not error)
- Error paths: unknown method, invalid PEM, public-key-only Sign attempt

### Wiki
The component wiki at gambaswiki.org/wiki/comp/gb.openssl is curated
manually (verified empirically: it diverges from /**G */ docstrings).
[If wiki MR opened: link here. Otherwise:]
**Wiki update needed**: the pages `pkey` and `signature` need to be
created, with particular attention to the explicit warning on
`Signature[m].Verify()` return-value contract (`FALSE` is the normal
"signature does not match" outcome, NOT an error — see the /**G */ in
c_signature.c). Draft content for both pages is available on request.
```

- [ ] **Step 3: Return the MR URL**

The MR URL is printed by `glab mr create` (or visible in the browser). Save it for follow-up. From here on, respond to Benoît's review comments by adding **new commits** to the branch (no force-push, no amend of published commits, per the strict upstream convention).

---

## Self-Review Checklist

Cross-check the spec sections against the plan:

| Spec section | Plan coverage |
|---|---|
| §Architettura — five descriptors | Task 2 (PKey scaffold), Task 11 (Signature scaffold) — 3 of 5 covered (PKey, Signature, .Signature.Method). The two PKeyCipher descriptors are explicitly out of scope for MR #1 — covered in MR #2 plan. ✓ |
| §Layout file — 4 new C files + 2 modified | Task 2 + Task 11 create all 4 new files, modify main.c and Makefile.am. ✓ |
| §Tabella Signature lookup | Task 11 includes the verbatim table. ✓ |
| §Default crittografici hardcoded | Task 13 sets PSS salt = DIGEST, padding hardcoded per table entry; Task 3 sets exponent default; Task 5 uses AES-256-CBC. ✓ |
| §API PKey descriptor | Generate (T3), properties (T4), LoadPrivate+ToPem (T5), encrypted PEM tests (T6), LoadPublic+ToPublicPem (T7), PublicPart (T8), DER methods (T9). ✓ |
| §API Signature + SignatureMethod descriptor | T11 (scaffold), T12 (List+IsSupported+_get), T13 (Sign+Verify). ✓ |
| §Modifica main.c | T2 (add CPKey + include c_pkey.h), T11 (decomment CSignature/CSignatureMethod + include c_signature.h). ✓ |
| §Gestione errori — Verify 1/0/<0 mapping | Task 13 implements the exact mapping. ✓ |
| §Documentazione livello 1 — /**G */ | T16 audits all and adds missing. Critical Verify warning in T13. ✓ |
| §Documentazione livello 2 — wiki | T17 checks feasibility and prepares content. ✓ |
| §Test plan MR #1 | T4-T15 cover all 13 listed tests (some bundled). ✓ |
| §Commit message format | T18 squashes to the exact body shown in the spec. ✓ |
| §MR description body | T19 has the verbatim template. ✓ |
| §Compatibilità OpenSSL 1.1+ | Plan uses only EVP_DigestSignInit/etc (1.1+ API), EVP_MD_CTX_new (1.1+). No 1.0.x compatibility code. ✓ |
| §Roadmap — only after MR#1 merged: MR#2 | Plan explicitly scopes only to MR#1, defers MR#2. ✓ |
| §Out-of-scope items (ECDSA, configurable params, etc.) | Not in plan. ✓ |

**Placeholder scan:** searching for "TODO", "TBD", "later" — only allowed placeholders are the RFC 7515 vector body in Task 15 (marked with "fetch the actual full PEM and signature ... at implementation time" because including 30 lines of base64 in the plan would be unreadable; the source RFC is linked verbatim). All other steps have complete content.

**Type consistency:** `PKey` (Gambas), `CPKey` (C descriptor), `CPKEY` (C struct), `_algo` (file-static of type `const struct sig_algo *`). All consistent across tasks. Method names: `PKey.Generate`, `.LoadPrivate`, `.LoadPublic`, `.LoadPrivateDer`, `.LoadPublicDer`, `.PublicPart`, `.Type`, `.Bits`, `.IsPrivate`, `.ToPem`, `.ToPublicPem`, `.ToDer`, `.ToPublicDer` — match spec verbatim. `Signature.List`, `.IsSupported`, indexer `_get`; `.Signature.Method.Sign(Data, Key)`, `.Verify(Data, Signature, Key)` — match.
