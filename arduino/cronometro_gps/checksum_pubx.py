#!/usr/bin/env python3
"""Ricalcola i checksum NMEA dei comandi PUBX usati nello sketch.

Il checksum NMEA e' lo XOR di tutti i caratteri compresi tra '$' e '*'
(esclusi), rappresentato in esadecimale maiuscolo a due cifre.
"""


def nmea_checksum(payload: str) -> str:
    x = 0
    for c in payload:
        x ^= ord(c)
    return f"{x:02X}"


PAYLOADS = [
    "PUBX,40,GLL,0,0,0,0,0,0",
    "PUBX,40,GSA,0,0,0,0,0,0",
    "PUBX,40,GSV,0,0,0,0,0,0",
    "PUBX,40,VTG,0,0,0,0,0,0",
    "PUBX,40,RMC,0,1,0,0,0,0",
    "PUBX,40,GGA,0,5,0,0,0,0",
]


if __name__ == "__main__":
    for p in PAYLOADS:
        print(f"${p}*{nmea_checksum(p)}")
