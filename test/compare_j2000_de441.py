#!/usr/bin/env python3
"""Compare intrinsic Earth and Moon series with DE441 in the fixed J2000 ecliptic.

No of-date frame, nutation, aberration, light-time, or epoch-anchor subtraction
is applied.  The engine's native planetary axes get only the constant
native->J2000 matrix from taiyin-lite/src/planet-frame.js.  The ELP-native
lunar L/B/R get their theory's own P/Q native->J2000 conversion, exactly as
taiyin-lite/src/moon-model.js does; this is necessary to express the ELP result
in J2000, not an additional of-date correction.  DE441 ICRS rectangular
vectors get only a constant J2000 obliquity tilt.  Both positions are geometric
at the same numeric JD (TT for the engine, TDB for the SPK; no periodic
TT-TDB correction is made).

Example:
    python3 test/compare_j2000_de441.py \
        --bsp /path/to/de441.bsp --start-year -1000 --end-year 1000 --step-days 32
Requires: jplephem (and its numpy dependency), a C99 compiler, DE441 locally.
"""

import argparse
import math
from pathlib import Path
import subprocess
import tempfile

from jplephem.spk import SPK

ROOT = Path(__file__).resolve().parents[1]
AU_KM = 149597870.7
ARCSEC_PER_RAD = 180.0 * 3600.0 / math.pi
J2000 = 2451545.0
JULIAN_YEAR_DAYS = 365.25

# A single constant rotation, copied from taiyin-lite/src/planet-frame.js.
# It maps the planetary theory's native axes to its fixed J2000 ecliptic.
# No time argument appears anywhere in this matrix.
NATIVE_TO_J2000 = (
    (0.9999999999999803, 1.9786988844634012e-7, 2.0200940272138296e-9),
    (-1.978698884606213e-7, 0.9999999999999803, 7.069560215011705e-9),
    (-2.020092628360702e-9, -7.0695604925674616e-9, 1.0),
)

# ELP's native-lunar-axes -> J2000 parameters.  Copied verbatim from
# taiyin-lite/src/moon-series.js, used as in moon-model.js:precession().
ELP_P = (0.000010180391, 4.7020439e-7, -5.417367e-10,
         -2.507948e-12, 4.63486e-15)
ELP_Q = (-0.000113469002, 1.2372674e-7, 1.265417e-9,
         -1.371808e-12, -3.20334e-15)

# Fixed J2000 obliquity; independent of the sample date.
EPSILON_J2000 = math.radians(84381.406 / 3600.0)
COS_EPS = math.cos(EPSILON_J2000)
SIN_EPS = math.sin(EPSILON_J2000)


def wrap_radians(value):
    return math.remainder(value, 2.0 * math.pi)


def spherical(vector):
    x, y, z = vector
    return (math.atan2(y, x), math.atan2(z, math.hypot(x, y)),
            math.hypot(math.hypot(x, y), z))


def engine_j2000(lon, lat, radius_au):
    cb = math.cos(lat)
    native = (radius_au * cb * math.cos(lon),
              radius_au * cb * math.sin(lon), radius_au * math.sin(lat))
    fixed = tuple(sum(row[i] * native[i] for i in range(3))
                  for row in NATIVE_TO_J2000)
    return spherical(fixed)


def polynomial(coefficients, x):
    value = 0.0
    for coefficient in reversed(coefficients):
        value = value * x + coefficient
    return value


def moon_j2000(jd, lon, lat, radius_km):
    """ELP-native L/B/R -> fixed J2000; same P/Q matrix as moon-model.js."""
    century = (jd - J2000) / 36525.0
    p = polynomial(ELP_P, century) * century
    q = polynomial(ELP_Q, century) * century
    norm = 1.0 - p*p - q*q
    if norm < 0.0:
        raise ValueError(f"ELP P/Q outside real-valued domain at JD {jd}")
    r = 2.0 * math.sqrt(norm)
    cb = math.cos(lat)
    x = radius_km * cb * math.cos(lon)
    y = radius_km * cb * math.sin(lon)
    z = radius_km * math.sin(lat)
    return spherical((
        (1.0 - 2.0*p*p)*x + 2.0*p*q*y + p*r*z,
        2.0*p*q*x + (1.0 - 2.0*q*q)*y - q*r*z,
        -p*r*x + q*r*y + (1.0 - 2.0*p*p - 2.0*q*q)*z,
    ))


def de441_j2000(vector_icrs_km):
    x, y, z = vector_icrs_km
    return spherical((x, COS_EPS * y + SIN_EPS * z,
                      -SIN_EPS * y + COS_EPS * z))


def compile_probe(compiler, output):
    cmd = [compiler, "-std=c99", "-O2", "-D_GNU_SOURCE", "-DSF_LUT_N=4096",
           "-I", str(ROOT / "core"), "-I", str(ROOT / "include"),
           "-o", str(output), str(ROOT / "test/j2000_native_probe.c"),
           str(ROOT / "core/sf_fixed.c"), "-lm"]
    subprocess.run(cmd, check=True)


def engine_samples(exe, jds):
    values = "".join(f"{jd:.10f}\n" for jd in jds)
    result = subprocess.run([str(exe)], input=values, text=True,
                            capture_output=True, check=True)
    lines = result.stdout.splitlines()
    if len(lines) != len(jds):
        raise RuntimeError(f"C probe returned {len(lines)} rows for {len(jds)} JDs")
    rows = [tuple(map(float, line.split())) for line in lines]
    if any(len(row) != 8 or abs(row[0] - jd) > 1e-7
           for row, jd in zip(rows, jds)):
        raise RuntimeError("C probe columns/JDs did not match the input")
    return rows


class Segments:
    def __init__(self, kernel):
        self.by_pair = {}
        for segment in kernel.segments:
            self.by_pair.setdefault((segment.center, segment.target), []).append(segment)
        for series in self.by_pair.values():
            series.sort(key=lambda segment: segment.start_jd)

    def position(self, center, target, jd):
        pair = (center, target)
        for segment in self.by_pair[pair]:
            if segment.start_jd <= jd <= segment.end_jd:
                return segment.compute(jd)
        raise ValueError(f"DE441 has no {pair} segment at JD {jd}")


def summarize(label, values):
    errors = [error for _, error in values]
    maximum_jd, maximum = max(values, key=lambda pair: abs(pair[1]))
    mean = sum(errors) / len(errors)
    rms = math.sqrt(sum(error * error for error in errors) / len(errors))
    ordered = sorted(abs(error) for error in errors)
    p95 = ordered[math.ceil(0.95 * len(ordered)) - 1]
    print(f"{label:18s} mean={mean:+10.4f} rms={rms:9.4f} "
          f"p95={p95:9.4f} max={abs(maximum):9.4f} "
          f"at JD={maximum_jd:.5f} (signed {maximum:+.4f})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bsp", type=Path, required=True, help="local de441.bsp")
    parser.add_argument("--start-year", type=float, default=-1000.0,
                        help="years relative to J2000 (default -1000)")
    parser.add_argument("--end-year", type=float, default=1000.0,
                        help="years relative to J2000 (default +1000)")
    parser.add_argument("--step-days", type=float, default=32.0)
    parser.add_argument("--cc", default="cc", help="C99 compiler")
    args = parser.parse_args()
    if not args.bsp.is_file():
        parser.error(f"DE441 file not found: {args.bsp}")
    if not (args.start_year < args.end_year and args.step_days > 0):
        parser.error("require start-year < end-year and step-days > 0")

    first = J2000 + args.start_year * JULIAN_YEAR_DAYS
    last = J2000 + args.end_year * JULIAN_YEAR_DAYS
    count = int(math.floor((last - first) / args.step_days)) + 1
    jds = [first + i * args.step_days for i in range(count)]
    if jds[-1] < last - 1e-8:
        jds.append(last)

    with tempfile.TemporaryDirectory(prefix="sf_j2000_") as temp:
        exe = Path(temp) / "probe"
        compile_probe(args.cc, exe)
        library = engine_samples(exe, jds)

    errors = {"earth_lon": [], "earth_lat": [], "earth_radius_full": [],
              "earth_radius_default": [], "moon_lon": [], "moon_lat": [],
              "moon_radius": []}
    with SPK.open(str(args.bsp)) as kernel:
        segments = Segments(kernel)
        for jd, (_, earth_l, earth_b, earth_r, earth_r_default,
                 moon_l, moon_b, moon_r) in zip(jds, library):
            # Earth heliocentric and Moon geocentric vectors in ICRS/km.
            earth_icrs = (segments.position(0, 3, jd)
                          + segments.position(3, 399, jd)
                          - segments.position(0, 10, jd))
            moon_icrs = (segments.position(3, 301, jd)
                         - segments.position(3, 399, jd))
            el, eb, er = engine_j2000(earth_l, earth_b, earth_r)
            jl, jb, jr = de441_j2000(earth_icrs)
            ml, mb, mr = moon_j2000(jd, moon_l, moon_b, moon_r)
            nl, nb, nr = de441_j2000(moon_icrs)
            errors["earth_lon"].append((jd, wrap_radians(el - jl) * ARCSEC_PER_RAD))
            errors["earth_lat"].append((jd, (eb - jb) * ARCSEC_PER_RAD))
            errors["earth_radius_full"].append((jd, er * AU_KM - jr))
            errors["earth_radius_default"].append((jd, earth_r_default * AU_KM - jr))
            errors["moon_lon"].append((jd, wrap_radians(ml - nl) * ARCSEC_PER_RAD))
            errors["moon_lat"].append((jd, (mb - nb) * ARCSEC_PER_RAD))
            errors["moon_radius"].append((jd, mr - nr))

    print(f"DE441: {args.bsp}\n"
          f"JDs: {len(jds)} from {jds[0]:.5f} to {jds[-1]:.5f} "
          f"(step {args.step_days:g} days)\n"
          "Frame: fixed J2000 ecliptic; geometric, same numeric JD; ELP-native "
          "P/Q included, no of-date or apparent corrections.\n"
          "Residual sign: solar-fast minus DE441.\n")
    summarize("Earth L (arcsec)", errors["earth_lon"])
    summarize("Earth B (arcsec)", errors["earth_lat"])
    summarize("Earth R full (km)", errors["earth_radius_full"])
    summarize("Earth R default", errors["earth_radius_default"])
    summarize("Moon L (arcsec)", errors["moon_lon"])
    summarize("Moon B (arcsec)", errors["moon_lat"])
    summarize("Moon R (km)", errors["moon_radius"])


if __name__ == "__main__":
    main()
