# Crypto++ version markers in `cryptlibR.lib`

Objects: 90; defined symbols: 9310.

Source paths embedded in anonymous-namespace symbols:

- `D:\Work\Sfc\crypto31\ecp.cpp`
- `D:\Work\Sfc\crypto31\mars.cpp`
- `D:\Work\Sfc\crypto31\oaep.cpp`
- `D:\Work\Sfc\crypto31\seal.cpp`
- `D:\Work\Sfc\crypto31\secshare.cpp`
- `D:\Work\Sfc\crypto31\twofish.cpp`

| Marker | Introduced/changed | Present |
|---|---|---|
| namespace CryptoPP | 3.0 added | yes (5167) |
| MD2 | 3.0 added | yes (6) |
| RC6 | 3.0 added | yes (3) |
| MARS | 3.0 added | yes (4) |
| ARC4 | 3.1 added | yes (4) |
| Rijndael | 3.1 added | yes (9) |
| Twofish | 3.1 added | yes (7) |
| Serpent | 3.1 added | yes (3) |
| DMAC | 3.1 added | no |
| CBC_MAC | 3.1 added | no |
| TripleDES (old name) | renamed in 3.2 | yes (4) |
| DES_EDE (old name) | renamed in 3.2 | no |
| DES_EDE2 / DES_EDE3 | 3.2 added | no |
| DES_XEX3 (DESX) | 3.2 added | no |
| ECDSA | 3.2 added | no |
| DefaultEncryptorWithMAC | 3.2 added | no |
| Skipjack | 4.0 added | no |
| CAST256 | 4.0 added | no |
| Panama | 4.0 added | no |
| SHA256/384/512 | 4.0 added | no |
| XTR | 4.0 added | no |
| Socket / WindowsPipe | 4.0 added | no |
| MARC4 | 4.1 added | no |
| MD4 | 4.2 added | no |
| ESIGN | 5.0 added | no |

Objects vs `cryptopp/src/cryptlib.dsp`: lib 90, dsp .cpp 90, only in lib [], only in dsp [].
