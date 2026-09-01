#!/usr/bin/env python3
"""Derive backend/data/cities.tsv from the GeoNames public dump.

The trips pass reverse-geocodes each trip centroid to a human place name
("Rome, Italy") for folder naming. Rather than ship the full multi-hundred-MB
GeoNames dump or hit the network at runtime, we bundle a trimmed, pre-joined
table of populated places.

Inputs (downloaded from https://download.geonames.org/export/dump/):
  - cities5000.zip      -> cities5000.txt  (places with population >= 5000)
  - countryInfo.txt     (ISO country code -> country name)
  - admin1CodesASCII.txt (admin1 code -> first-order division name)

Output: backend/data/cities.tsv, one line per place, tab-separated:
  ascii_name <tab> lat <tab> lon <tab> country_name <tab> admin1_name <tab> population

Sorted by descending population so the loader can bias ties toward the more
prominent place. No header row; UTF-8; '\n' line endings.

Usage:
  cd "$(mktemp -d)"
  curl -sSLO https://download.geonames.org/export/dump/cities5000.zip
  curl -sSLO https://download.geonames.org/export/dump/countryInfo.txt
  curl -sSLO https://download.geonames.org/export/dump/admin1CodesASCII.txt
  unzip -o cities5000.zip
  python3 /path/to/backend/tools/trim_geonames.py \
      --cities cities5000.txt --countries countryInfo.txt \
      --admin1 admin1CodesASCII.txt \
      --out /path/to/backend/data/cities.tsv
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path


def load_countries(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    with path.open(encoding="utf-8") as handle:
        for line in handle:
            if line.startswith("#") or not line.strip():
                continue
            cols = line.rstrip("\n").split("\t")
            if len(cols) < 5:
                continue
            iso, name = cols[0], cols[4]
            if iso and name:
                out[iso] = name
    return out


def load_admin1(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    with path.open(encoding="utf-8") as handle:
        for line in handle:
            cols = line.rstrip("\n").split("\t")
            if len(cols) < 2:
                continue
            out[cols[0]] = cols[1]
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cities", required=True, type=Path)
    parser.add_argument("--countries", required=True, type=Path)
    parser.add_argument("--admin1", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    countries = load_countries(args.countries)
    admin1 = load_admin1(args.admin1)

    rows: list[tuple[str, float, float, str, str, int]] = []
    with args.cities.open(encoding="utf-8", newline="") as handle:
        reader = csv.reader(handle, delimiter="\t", quoting=csv.QUOTE_NONE)
        for cols in reader:
            if len(cols) < 15:
                continue
            ascii_name = cols[2].strip()
            if not ascii_name:
                continue
            try:
                lat = float(cols[4])
                lon = float(cols[5])
            except ValueError:
                continue
            country_code = cols[8].strip()
            admin1_code = cols[10].strip()
            try:
                population = int(cols[14] or "0")
            except ValueError:
                population = 0
            country_name = countries.get(country_code, country_code)
            admin1_name = admin1.get(f"{country_code}.{admin1_code}", "")
            rows.append(
                (ascii_name, lat, lon, country_name, admin1_name, population)
            )

    rows.sort(key=lambda r: r[5], reverse=True)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", newline="") as handle:
        for ascii_name, lat, lon, country_name, admin1_name, population in rows:
            handle.write(
                f"{ascii_name}\t{lat:.5f}\t{lon:.5f}\t{country_name}\t"
                f"{admin1_name}\t{population}\n"
            )

    print(f"wrote {len(rows)} places to {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
