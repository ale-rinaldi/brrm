# gb.openssl: supporto RSA (firma, cifratura, gestione chiavi)

**Status:** design approvato dall'utente
**Target upstream:** https://gitlab.com/gambas/gambas — componente `gb.openssl`
**Autore:** Alessandro Rinaldi
**Data:** 2026-05-12

## Contesto e motivazione

Il componente `gb.openssl` di Gambas espone digest (`Digest`), cifratura simmetrica (`Cipher` + `CipherText`), HMAC (`HMac`) e utility (`OpenSSL.RandomBytes`, `Pbkdf2`, `Scrypt`). Non espone nessuna operazione asimmetrica. Chi ha bisogno di firme RSA (es. JWT per Service Account Google) ricorre a `Shell openssl dgst -sign`.

In `gb.openssl/src/main.c` esiste dal 2013 un placeholder commentato:

```c
GB_DESC *GB_CLASSES[] EXPORT = {
    COpenSSL, CDigest, CDigestMethod,
    CCipher, CCipherMethod, CCipherText, CHMac,
//  CSignature,
//  CSignatureMethod,
    NULL
};
```

Il design completa questa TODO di Tobias Boege ed estende la copertura RSA con cifratura asimmetrica e una classe per la gestione delle chiavi.

## Strategia di delivery: due MR sequenziali

| MR | Titolo | Contenuto | Note strategiche |
|----|--------|-----------|------------------|
| #1 | `gb.openssl: Add RSA signature support and PKey class` | `c_pkey.{c,h}`, `c_signature.{c,h}`, modifiche a `main.c` e `Makefile.am`, test in `openssl-test/.src/Main.module` | Riempie esattamente il placeholder `CSignature` + `CSignatureMethod` di Tobias. Aggiunge `PKey` come supporting class (necessaria perché `Sign/Verify` ricevono `PKey` come argomento). Risolve il caso JWT al 100%. |
| #2 | `gb.openssl: Add PKeyCipher class for RSA public-key encryption` | `c_pkeycipher.{c,h}`, modifiche a `main.c` e `Makefile.am`, test addizionali | Da aprire **dopo** l'accettazione di #1. Si appoggia su `PKey`. Ortogonale alle firme. |

**Motivazione del taglio.** Stima totale ~800-1200 righe di C nuove. MR di taglia simile da contributori non-regolari sono storicamente parcheggiate o chiuse (`gb.gmp` !315, `gb.media.mpris` !326, ecc.). Due MR di ~500-700 righe ciascuna sono molto più digeribili. Se #1 viene rifattorizzata dal maintainer durante il review, lo stile preciso emerge prima che si investa lavoro su #2.

`PKey` non viene tagliata in una MR a sé perché senza un consumatore (Signature o PKeyCipher) sarebbe codice inerte nel componente: difficile da motivare nel commit body.

## Architettura

### Cinque descriptor C (tre classi Gambas)

| Classe Gambas | Descriptor C | Tipo | MR | Ruolo |
|---|---|---|---|---|
| `PKey` | `CPKey` | `GB_NOT_CREATABLE`, instance con `_free` | #1 | Incapsula `EVP_PKEY *`. Factory statici, properties, import/export PEM/DER. |
| `Signature` | `CSignature` | `GB_NOT_CREATABLE` factory | #1 | Indexer `Signature["rsa-pss-sha256"]` → method object. Mirror di `Cipher`/`Digest`. |
| `.Signature.Method` | `CSignatureMethod` | `GB_VIRTUAL_CLASS` | #1 | Operazioni `Sign(Data, Key)`, `Verify(Data, Signature, Key)`. |
| `PKeyCipher` | `CPKeyCipher` | `GB_NOT_CREATABLE` factory | #2 | Indexer `PKeyCipher["rsa-oaep-sha256"]` → method object. |
| `.PKeyCipher.Method` | `CPKeyCipherMethod` | `GB_VIRTUAL_CLASS` | #2 | Operazioni `Encrypt(Data, Key)`, `Decrypt(Data, Key)`. |

Nessuna modifica a classi esistenti (`Digest`, `Cipher`, `HMac`, `OpenSSL`). L'estensione è puramente additiva.

### Dipendenze tra classi

```
   PKey (autonoma)
     ↑ usata come argomento da
   Signature   PKeyCipher
   ↓ creano    ↓ creano
   .Signature.Method   .PKeyCipher.Method
```

### Layout file

```
gb.openssl/src/
├── c_pkey.c       / c_pkey.h          ← nuovi (MR #1)
├── c_signature.c  / c_signature.h     ← nuovi (MR #1)
├── c_pkeycipher.c / c_pkeycipher.h    ← nuovi (MR #2)
├── main.c                             ← modificato in entrambe le MR
└── Makefile.am                        ← modificato in entrambe le MR
gb.openssl/openssl-test/.src/Main.module ← esteso in entrambe le MR
```

## Convenzione del method string

L'indexer di `Signature` e `PKeyCipher` codifica nella stringa **algoritmo + padding + hash**, parallelo a `Cipher["aes-256-cbc"]` che codifica algoritmo+keylength+mode.

### Tabella `Signature` (MR #1)

```c
static const struct sig_algo {
    const char *name;
    int         key_type;
    int         padding;
    const char *md_name;
} SIG_ALGOS[] = {
    { "rsa-pkcs1-sha256", EVP_PKEY_RSA, RSA_PKCS1_PADDING,     "sha256" },
    { "rsa-pkcs1-sha384", EVP_PKEY_RSA, RSA_PKCS1_PADDING,     "sha384" },
    { "rsa-pkcs1-sha512", EVP_PKEY_RSA, RSA_PKCS1_PADDING,     "sha512" },
    { "rsa-pss-sha256",   EVP_PKEY_RSA, RSA_PKCS1_PSS_PADDING, "sha256" },
    { "rsa-pss-sha384",   EVP_PKEY_RSA, RSA_PKCS1_PSS_PADDING, "sha384" },
    { "rsa-pss-sha512",   EVP_PKEY_RSA, RSA_PKCS1_PSS_PADDING, "sha512" },
    { NULL, 0, 0, NULL }
};
```

Lookup lineare via `strcasecmp` (~8 righe). `Signature.List` itera la tabella e ritorna gli `name`. `IsSupported` riusa la stessa lookup. `_get` salva l'entry trovata nel virtual object.

### Tabella `PKeyCipher` (MR #2)

```c
static const struct pkc_algo {
    const char *name;
    int         key_type;
    int         padding;
    const char *md_name;       // NULL per PKCS#1 v1.5 (no hash); altrimenti hash OAEP
} PKC_ALGOS[] = {
    { "rsa-pkcs1",       EVP_PKEY_RSA, RSA_PKCS1_PADDING,      NULL     },
    { "rsa-oaep-sha1",   EVP_PKEY_RSA, RSA_PKCS1_OAEP_PADDING, "sha1"   },
    { "rsa-oaep-sha256", EVP_PKEY_RSA, RSA_PKCS1_OAEP_PADDING, "sha256" },
    { NULL, 0, 0, NULL }
};
```

## Default crittografici hardcoded

Per minimizzare la superficie API in queste prime due MR, i parametri fini sono hardcoded sui valori sicuri di riferimento. Estensioni con parametri opzionali potranno arrivare in una MR successiva se emerge un caso reale.

| Parametro | Valore |
|---|---|
| PSS salt length | `digest size` (RFC 7518 §3.5) |
| OAEP MGF1 hash | uguale all'hash OAEP scelto |
| OAEP label | assente |
| RSA exponent (Generate) | 65537 |
| Cifratura PEM (passphrase) | AES-256-CBC |
| Formato chiave privata DER | PKCS#8 |
| Formato chiave pubblica DER | SPKI (X.509 SubjectPublicKeyInfo) |

## API: descriptors completi

### MR #1: `PKey`

```c
GB_DESC CPKey[] = {
    GB_DECLARE("PKey", sizeof(CPKEY)),
    GB_NOT_CREATABLE(),

    GB_STATIC_METHOD("Generate",       "PKey", PKey_Generate,       "(Bits)i"),
    GB_STATIC_METHOD("LoadPrivate",    "PKey", PKey_LoadPrivate,    "(Pem)s[(Passphrase)s]"),
    GB_STATIC_METHOD("LoadPublic",     "PKey", PKey_LoadPublic,     "(Pem)s"),
    GB_STATIC_METHOD("LoadPrivateDer", "PKey", PKey_LoadPrivateDer, "(Der)s"),
    GB_STATIC_METHOD("LoadPublicDer",  "PKey", PKey_LoadPublicDer,  "(Der)s"),

    GB_PROPERTY_READ("Type",       "s",    PKey_Type),
    GB_PROPERTY_READ("Bits",       "i",    PKey_Bits),
    GB_PROPERTY_READ("IsPrivate",  "b",    PKey_IsPrivate),
    GB_PROPERTY_READ("PublicPart", "PKey", PKey_PublicPart),

    GB_METHOD("ToPem",       "s", PKey_ToPem,       "[(Passphrase)s]"),
    GB_METHOD("ToPublicPem", "s", PKey_ToPublicPem, NULL),
    GB_METHOD("ToDer",       "s", PKey_ToDer,       NULL),
    GB_METHOD("ToPublicDer", "s", PKey_ToPublicDer, NULL),

    GB_METHOD("_free", NULL, PKey_free, NULL),

    GB_END_DECLARE
};
```

Struttura interna:
```c
typedef struct {
    GB_BASE   ob;
    EVP_PKEY *pkey;
} CPKEY;
#define THIS ((CPKEY *) _object)
```

Allocazione C-side via `GB.New(GB.FindClass("PKey"), NULL, NULL)` nei factory statici (precedente: `CipherMethod_Encrypt` lo fa per `CipherText`).

**Nota implementativa**: il precedente `CipherText` ha un `_new` Gambas-callable, mentre `PKey` è `GB_NOT_CREATABLE`. Da verificare al primo build se `GB.New` con NULL/NULL funziona su una classe `GB_NOT_CREATABLE` priva di `_new`. Se non funziona, si aggiunge un `_new` no-op nel descriptor con `0` argomenti dichiarati.

### MR #1: `Signature` + `.Signature.Method`

```c
GB_DESC CSignature[] = {
    GB_DECLARE("Signature", 0),
    GB_NOT_CREATABLE(),

    GB_STATIC_PROPERTY_READ("List", "String[]", Signature_List),

    GB_STATIC_METHOD("_init", NULL, Signature_init, NULL),
    GB_STATIC_METHOD("_exit", NULL, Signature_exit, NULL),
    GB_STATIC_METHOD("_get", ".Signature.Method", Signature_get, "(Method)s"),
    GB_STATIC_METHOD("IsSupported", "b", Signature_IsSupported, "(Method)s"),

    GB_END_DECLARE
};

GB_DESC CSignatureMethod[] = {
    GB_DECLARE(".Signature.Method", 0),
    GB_VIRTUAL_CLASS(),

    GB_STATIC_METHOD("Sign",   "s", SignatureMethod_Sign,   "(Data)s(Key)PKey"),
    GB_STATIC_METHOD("Verify", "b", SignatureMethod_Verify, "(Data)s(Signature)s(Key)PKey"),

    GB_END_DECLARE
};
```

### MR #2: `PKeyCipher` + `.PKeyCipher.Method`

```c
GB_DESC CPKeyCipher[] = {
    GB_DECLARE("PKeyCipher", 0),
    GB_NOT_CREATABLE(),

    GB_STATIC_PROPERTY_READ("List", "String[]", PKeyCipher_List),

    GB_STATIC_METHOD("_init", NULL, PKeyCipher_init, NULL),
    GB_STATIC_METHOD("_exit", NULL, PKeyCipher_exit, NULL),
    GB_STATIC_METHOD("_get", ".PKeyCipher.Method", PKeyCipher_get, "(Method)s"),
    GB_STATIC_METHOD("IsSupported", "b", PKeyCipher_IsSupported, "(Method)s"),

    GB_END_DECLARE
};

GB_DESC CPKeyCipherMethod[] = {
    GB_DECLARE(".PKeyCipher.Method", 0),
    GB_VIRTUAL_CLASS(),

    GB_STATIC_METHOD("Encrypt", "s", PKeyCipherMethod_Encrypt, "(Data)s(Key)PKey"),
    GB_STATIC_METHOD("Decrypt", "s", PKeyCipherMethod_Decrypt, "(Data)s(Key)PKey"),

    GB_END_DECLARE
};
```

### Modifica a `main.c` (entrambe le MR)

```c
#include "c_pkey.h"        // MR #1
#include "c_signature.h"   // MR #1
#include "c_pkeycipher.h"  // MR #2

GB_DESC *GB_CLASSES[] EXPORT = {
    COpenSSL,
    CDigest, CDigestMethod,
    CCipher, CCipherMethod, CCipherText,
    CHMac,
    CPKey,                                        // MR #1
    CSignature, CSignatureMethod,                 // MR #1 (decommentate)
    CPKeyCipher, CPKeyCipherMethod,               // MR #2
    NULL
};
```

## Gestione errori

Convenzione condivisa con il resto del componente:

| Situazione | Comportamento |
|---|---|
| Argomento invalido (es. `Bits=0`, metodo sconosciuto) | `GB.Error("messaggio letterale")` |
| Errore OpenSSL (chiave malformata, padding incompatibile) | `MAIN_error("RSA <op> failed: &1")` — il `&1` viene rimpiazzato con `ERR_error_string(ERR_get_error(), NULL)` |
| `Verify` con firma che non matcha | **Ritorna `FALSE`**, non solleva eccezione. È l'esito normale della verifica. |

`EVP_DigestVerifyFinal` di OpenSSL ritorna `1`/`0`/`<0`. Mapping:
- `1` → `GB.ReturnBoolean(TRUE)`
- `0` → `GB.ReturnBoolean(FALSE)` (firma non valida, normale)
- `<0` → `MAIN_error("RSA verify failed: &1")` (errore tecnico)

## Documentazione

Due livelli, **entrambi necessari**:

### 1. In-source `/**G */` (committate con la MR)

Blocchi `/**G ... **/` sopra ogni `BEGIN_METHOD`/`BEGIN_PROPERTY` e dentro `GB_DESC`. Pattern verificato in `c_digest.c` e `c_cipher.c`. Documentazione canonica del componente.

**Caso critico — `Verify`:** il return value è semanticamente sovraccarico (TRUE/FALSE entrambi sono esiti validi della verifica). La `/**G */` DEVE esplicitare che "nessun errore != firma valida", per evitare il footgun "uso `Verify` come void e procedo come se fosse OK".

```c
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
```

Esempi più brevi per gli altri metodi seguono il pattern di `c_digest.c`/`c_cipher.c` (2-6 righe ciascuno).

### 2. Wiki a `gambaswiki.org`

Verificato empiricamente: NON è auto-generato dalle `/**G */`. La pagina wiki di `Digest` ha testi diversi dal sorgente e alcuni metodi (es. `Hash`) sono assenti. È curata a mano, presumibilmente da Benoît Minisini.

**Processo previsto**:
1. **Prima** di aprire la MR #1, verificare se il wiki accetta MR esterne / edit di non-maintainer
2. Se sì → aprire una MR sul wiki con le pagine `comp/gb.openssl/pkey` e `comp/gb.openssl/signature` (e nel ciclo MR #2: `comp/gb.openssl/pkeycipher`), referenziandola dalla MR di codice
3. Se no → nel body della MR di codice, sezione dedicata "Wiki update needed" che evidenzia esplicitamente l'esigenza, con il testo proposto delle pagine già pronto

In entrambi i casi il warning su `Verify` deve apparire **anche** sul wiki, non solo nel sorgente.

## Test

Pattern verificato in `openssl-test/.src/Main.module`: `Test.Plan(N)` con N assertion `Assert.Equals`/`Assert.True`/`Assert.False`. Test invocati dal global runner `gambas3-selftest` via `./test-fast` (top-level del repo). Nessuna integrazione `Makefile.am` necessaria.

### Test MR #1 (~13 nuove assertion)

```gambas
Test_PKey_GenerateAndProperties()           ' Generate(2048): Bits=2048, Type="RSA", IsPrivate=True
Test_PKey_PemRoundTrip()                    ' Generate → ToPem → LoadPrivate → ToPem: stesso output
Test_PKey_DerRoundTrip()                    ' come sopra ma DER
Test_PKey_EncryptedPemRoundTrip()           ' ToPem(passphrase) → LoadPrivate(pem, passphrase)
Test_PKey_PublicPartExtraction()            ' PublicPart.IsPrivate=False, e firma+verify funziona
Test_PKey_LoadInvalidPem_RaisesError()      ' Assert.Error su PEM malformato
Test_Signature_RsaPkcs1Sha256_SignVerify()  ' round-trip per ognuno dei 6 metodi
Test_Signature_RsaPssSha256_SignVerify()
Test_Signature_VerifyKnownVector_Jwt()      ' vettore noto da RFC 7515 Appendix A.2 (RS256)
Test_Signature_VerifyTamperedData_ReturnsFalse()
Test_Signature_VerifyWrongKey_ReturnsFalse()
Test_Signature_UnknownMethod_RaisesError()
Test_Signature_List_ContainsExpectedMethods()
```

Vettore JWT canonico (RFC 7515 §A.2) come golden test: chiave RSA pubblica + payload base64url + signature base64url noti, deve dare `Verify = TRUE`.

### Test MR #2 (~6 nuove assertion)

```gambas
Test_PKeyCipher_Pkcs1_RoundTrip()
Test_PKeyCipher_OaepSha1_RoundTrip()
Test_PKeyCipher_OaepSha256_RoundTrip()
Test_PKeyCipher_DecryptWithWrongKey_RaisesError()  ' OpenSSL fallisce hard, non FALSE
Test_PKeyCipher_DataTooLong_RaisesError()          ' dato > modulus - padding overhead
Test_PKeyCipher_UnknownMethod_RaisesError()
```

## Commit message (MR #1)

Formato verificato su master (es. commit `3f15e2b0`, `6cc19d35`):

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

## Commit message (MR #2)

```
gb.openssl: Add PKeyCipher class for RSA public-key encryption

[GB.OPENSSL]
* NEW: PKeyCipher: New class for asymmetric encryption, mirroring
  the Cipher factory pattern. Supports RSA with PKCS#1 v1.5 and
  OAEP (SHA-1, SHA-256) padding schemes.
```

## MR description body (MR #1)

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
  matches OAEP hash, exponent = 65537, encrypted PEM = AES-256-CBC.
  Exposing these as parameters can be a follow-up MR if needed.
- A follow-up MR will add `PKeyCipher` for RSA public-key encryption
  (PKCS#1 v1.5 / OAEP). Keeping these separate to ease review.

### Tests
Adds 13 self-tests to `openssl-test/Main.module`, including:
- Round-trip PEM/DER import/export, with and without passphrase
- Sign/Verify round-trip for all 6 RSA signature variants
- Known-vector verification from RFC 7515 Appendix A.2 (JWT RS256)
- Tampered-data and wrong-key verification (must return FALSE)
- Error paths: unknown method, invalid PEM, etc.

### Wiki
The component wiki at gambaswiki.org/wiki/comp/gb.openssl is curated
manually and is NOT auto-generated from /**G */ docstrings. [If wiki
MR opened: link here]. Otherwise: the pages `pkey` and `signature`
need to be created, with particular attention to the explicit warning
on `Signature[m].Verify()` return-value contract (see /**G */ in
c_signature.c).
```

## Compatibilità

- **OpenSSL**: target 1.1.0+ (allineato col resto del componente). Si verifica nel codice di Tobias se sono presenti guards `#if OPENSSL_VERSION_NUMBER < 0x10100000L`: se sì, si mantengono; se assenti, si aggiunge un `#error` per versioni precedenti.
- **Gambas**: nessuna API nuova richiesta dal binding (tutto via `GB.Error`, `MAIN_error`, `GB.New`, `GB.ReturnXxx` già esistenti).
- **Build system**: solo aggiunte a `gb_openssl_la_SOURCES` in `Makefile.am`. Niente `configure.ac` modifications.

## Rischi e mitigazioni

| Rischio | Probabilità | Mitigazione |
|---|---|---|
| Benoît rifiuta o riscrive l'API design | Media | Adottato il pattern esatto di Cipher/Digest. Riempito un placeholder esistente. MR contenuta. |
| MR parcheggiata indefinitamente | Media | Commit message ricco di motivazione (riferimento al placeholder). Test completi. Due MR piccole invece di una grande. |
| Conflitti con OpenSSL 3.0 deprecation warnings | Bassa | EVP_DigestSign/Verify API è stabile da 1.1 a 3.x. Nessun uso di API low-level deprecate (RSA_sign, ecc.). |
| Wiki non aggiornata → footgun su Verify | Media | Warning forte nelle `/**G */`. Esempio di `If ... Verify(...) Then ...` direttamente in docstring. Tentativo di MR su wiki se possibile. |
| OAEP MGF1 hardcoded non-matching per casi reali futuri | Bassa | Default OpenSSL coincide col 99% degli usi. Estensione in MR futura se serve. |

## Roadmap di implementazione

1. **Workspace** — clone di `gitlab.com/gambas/gambas`, branch feature da master
2. **MR #1 — PKey** — `c_pkey.{c,h}` con factory minimi e `_free`; build verifica
3. **MR #1 — Signature** — `c_signature.{c,h}` con tabella lookup e Sign/Verify
4. **MR #1 — wiring** — modifiche a `main.c` e `Makefile.am`
5. **MR #1 — test** — assertion in `openssl-test/.src/Main.module`, vettore JWT da RFC 7515
6. **MR #1 — docstring** — `/**G */` complete, con warning esplicito su Verify
7. **MR #1 — wiki check** — tentativo di MR sul wiki; se non possibile, sezione dedicata nel body MR
8. **MR #1 — push e MR** — commit con format `[GB.OPENSSL]` block, body come da template
9. **Iterazione su feedback Benoît** — fix in commit aggiuntivi (no force-push, no amend di commit pubblicati)
10. **Solo dopo merge MR #1**: MR #2 — `PKeyCipher` (analoga, basata sul lavoro merged)

## Decisioni esplicitamente FUORI scope (per ora)

- ECDSA, Ed25519, Ed448 — il design lascia spazio (`SIG_ALGOS` table estendibile), ma non li implementa
- Parametri OAEP/PSS configurabili — MR futura se emerge un caso reale
- Streaming sign/verify (`EVP_DigestSignUpdate` su file grandi) — MR futura
- Parsing certificati X.509, CSR, ASN.1 — fuori scope di "RSA"
- Key wrapping, PKCS#7, CMS — fuori scope
- Supporto OpenSSL 3.0 provider API moderni (`EVP_SIGNATURE_fetch`) — sufficiente la 1.1 API
