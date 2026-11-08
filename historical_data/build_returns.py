#!/usr/bin/env python3
"""Build annual asset-class returns in Canadian dollars from the raw source
files in this directory.

Usage (from the repo root):
    python3 historical_data/build_returns.py [--check] \
        [--blend ca_eq=0.2,us_eq=0.2,intl_eq=0.2,ca_bonds=0.3,cash=0.1 OUT.csv]

Writes:
    historical_returns_cad.csv   year,ca_eq,us_eq,intl_eq,ca_bonds,cash,cpi
and, with --blend, a 3-column year,return_pct,cpi_pct file that the current
Monte Carlo loader reads (portfolio rebalanced to the weights each year).
--check prints cross-checks between overlapping sources.

All returns are nominal, total return (dividends/interest reinvested), in
percent, calendar year. Requires pandas, openpyxl and xlrd.

Sources and splice points (see also the CSV header this script writes):
  ca_eq     1957-1998 Statistics Canada 10-10-0125-01: S&P/TSX Composite
                      December close-to-close price change plus the average
                      monthly dividend yield for the year.
            1999-2025 MSCI Canada (gross, USD) converted to CAD.
  us_eq     1957-1998 Damodaran (NYU Stern) S&P 500 incl. dividends, USD,
                      converted to CAD.
            1999-2025 MSCI USA (gross, USD) converted to CAD.
  intl_eq   1957-1998 STAND-IN: GDP-weighted average of the 15 non-US,
                      non-Canada countries in the Jorda-Schularick-Taylor
                      Macrohistory Database R6 (equity total return, local
                      currency converted to USD, then CAD). Tracks MSCI EAFE
                      closely year to year but ran ~2 points/yr higher over
                      1999-2020.
            1999-2025 MSCI EAFE (gross, USD) converted to CAD.
  ca_bonds  1957-2025 APPROXIMATION from Statistics Canada 10-10-0122-01
                      Government of Canada marketable bond average yields
                      (1-3, 3-5, 5-10, over 10 years): each month, a par bond
                      at last month's yield is repriced at this month's
                      yield; the four maturity buckets are equally weighted
                      (average duration ~6, similar to a broad bond fund).
                      Government bonds only: no corporate/provincial spread.
  cash      1957-1962 JST short-term interest rate for Canada (annual avg).
            1963-2025 Statistics Canada 10-10-0122-01 3-month T-bill yield,
                      compounded monthly (4 blank months in 1970-1973
                      interpolated from neighbouring months).
  cpi       Canadian CPI annual % change, copied from ../tsx.csv
            (1987-2025 Perplexity/World Bank, 1960-1986 World Bank,
            1957-1959 Statistics Canada).
  FX        Year-end CAD per USD changes: 2001-2025 from MSCI Canada CAD
            level / USD level; 1957-2000 JST Canada exchange rate.
"""
import csv
import os
import sys

import numpy as np
import pandas as pd

HERE = os.path.dirname(os.path.abspath(__file__))
FIRST, LAST = 1957, 2025
OUT = os.path.join(HERE, "..", "historical_returns_cad.csv")


def path(name):
    return os.path.join(HERE, name)


# ---------------------------------------------------------------- readers
def read_statcan(name, label_prefix):
    """Return {series label: monthly pd.Series} from a StatCan 'as displayed'
    CSV export (months across columns)."""
    rows = list(csv.reader(open(path(name), encoding="utf-8-sig")))
    hdr_i = next(i for i, r in enumerate(rows)
                 if len(r) > 12 and r[1].split(" ")[0] in
                 ("January", "February", "March", "April", "May", "June", "July",
                  "August", "September", "October", "November", "December"))
    months = pd.to_datetime(rows[hdr_i][1:], format="%B %Y")
    out = {}
    for r in rows[hdr_i + 1:]:
        # data rows have a series label in column 0; unit rows ("Percent",
        # "Index, 2000=1000") have an empty column 0
        if r and r[0] and r[0].startswith(label_prefix) and len(r) > len(months) // 2:
            vals = []
            for v in r[1:len(months) + 1]:
                v = v.replace(",", "").rstrip("tABCDEF")
                vals.append(float(v) if v and not v.startswith("..") and v != "-99999" else np.nan)
            out[r[0]] = pd.Series(vals, index=months)
    return out


def msci_dec(name):
    """December month-end index levels from an MSCI end-of-day export."""
    x = pd.read_excel(path(name), header=None, skiprows=6).iloc[:, :2].dropna()
    x.columns = ["date", "lvl"]
    x["date"] = pd.to_datetime(x["date"])
    dec = x[x.date.dt.month == 12]
    return pd.Series(dec.lvl.values, index=dec.date.dt.year.values)


def annual(levels):
    return (levels / levels.shift(1) - 1) * 100


def find(prefix):
    return next(f for f in os.listdir(HERE) if f.startswith(prefix))


# ---------------------------------------------------------------- sources
jst = pd.read_excel(path("JSTdatasetR6.xlsx"))
jst_ca = jst[jst.country == "Canada"].set_index("year")

can_usd = msci_dec(find("912400 - MSCI Canada Index  - FULL - 1998"))
can_cad = msci_dec(find("912400 - MSCI Canada Index  - FULL - 2000"))
usa_usd = msci_dec(find("984000 - MSCI USA"))
eafe_usd = msci_dec(find("990300 - MSCI EAFE"))

# Year-end CAD per USD. MSCI's CAD and USD levels each have their own base,
# so their ratio is the exchange rate times a constant: use its year-over-year
# CHANGE (2001 on), and JST's year-end rate changes before that.
fx_msci = (can_cad / can_usd).dropna()
fx_chg = pd.concat([(jst_ca.xrusd / jst_ca.xrusd.shift(1)).loc[:2000],
                    (fx_msci / fx_msci.shift(1)).loc[2001:]])


def to_cad(usd_pct):
    return ((1 + usd_pct / 100) * fx_chg.reindex(usd_pct.index) - 1) * 100


# --- Canadian equities
tsx = read_statcan("1010012501-eng.csv", "")
close = next(v for k, v in tsx.items() if k.startswith("Standard and Poor"))
divy = next(v for k, v in tsx.items() if "dividend yields" in k)
close_dec = close[close.index.month == 12]
close_dec.index = close_dec.index.year
statcan_tsx = annual(close_dec) + divy.groupby(divy.index.year).mean()
msci_can = to_cad(annual(can_usd))
ca_eq = pd.concat([statcan_tsx.loc[FIRST:1998], msci_can.loc[1999:LAST]])

# --- US equities
dm = pd.read_excel(path("histretSP.xls"), sheet_name="Returns by year", header=None)
hdr = dm.index[dm.iloc[:, 0].astype(str).str.strip().eq("Year")][0]
dm = dm.iloc[hdr + 1:, :2]
dm.columns = ["year", "sp"]
dm = dm[pd.to_numeric(dm.year, errors="coerce").notna()]
sp_usd = pd.Series(dm.sp.astype(float).values * 100, index=dm.year.astype(int).values)
us_eq = pd.concat([to_cad(sp_usd).loc[FIRST:1998], to_cad(annual(usa_usd)).loc[1999:LAST]])

# --- International equities
intl = jst[~jst.country.isin(["USA", "Canada"])].copy().sort_values(["country", "year"])
intl["xr_prev"] = intl.groupby("country").xrusd.shift(1)
intl["eq_usd"] = (1 + intl.eq_tr) * intl.xr_prev / intl.xrusd - 1
intl["w"] = intl.groupby("country").gdp.shift(1) / intl.xr_prev   # prior-year GDP in USD
x = intl.dropna(subset=["eq_usd", "w"])
jst_intl_usd = x.groupby("year").apply(
    lambda g: np.average(g.eq_usd, weights=g.w) * 100, include_groups=False)
intl_eq = pd.concat([to_cad(jst_intl_usd).loc[FIRST:1998], to_cad(annual(eafe_usd)).loc[1999:LAST]])

# --- Canadian bonds (approximated from yields)
rates = read_statcan("1010012201-eng.csv", "")


def par_bond_month(y_prev, y_now, maturity):
    """One-month total return (%) of a par bond (semi-annual coupons at
    y_prev) repriced at y_now, maturity held constant (rolling index)."""
    c = y_prev / 100 / 2
    r = y_now / 100 / 2
    n = int(round(maturity * 2))
    price = 100.0 if r == c else (c * 100 * (1 - (1 + r) ** -n) / r + 100 * (1 + r) ** -n)
    return (price - 100) + y_prev / 12


buckets = {"1-3 year": 2.0, "3-5 year": 4.0, "5-10 year": 7.5, "over 10 years": 20.0}
bucket_monthly = []
for key, mat in buckets.items():
    s = next(v for k, v in rates.items() if k.startswith(
        "Government of Canada marketable bonds, average yield: " + key))
    m = pd.Series([par_bond_month(a, b, mat) for a, b in zip(s.shift(1), s)], index=s.index)
    bucket_monthly.append(m)
bond_m = pd.concat(bucket_monthly, axis=1).mean(axis=1, skipna=False)
bond_m = bond_m.dropna()
ca_bonds = (1 + bond_m / 100).groupby(bond_m.index.year).prod().sub(1).mul(100)
ca_bonds = ca_bonds[bond_m.groupby(bond_m.index.year).size() == 12]

# --- Cash
tb = next(v for k, v in rates.items() if k.startswith("Treasury bills: 3 month"))
# StatCan leaves four single months blank (Feb 1970, Nov 1970, Feb 1971,
# Apr 1973); fill each from its neighbours.
tb = tb.loc[tb.first_valid_index():].interpolate(limit=1, limit_area="inside")
tb_m = (tb.shift(1) / 1200).dropna()             # earn last month's yield
cash_tb = (1 + tb_m).groupby(tb_m.index.year).prod().sub(1).mul(100)
cash_tb = cash_tb[tb_m.groupby(tb_m.index.year).size() == 12]
cash = pd.concat([jst_ca.stir.loc[FIRST:1962], cash_tb.loc[1963:LAST]])

# --- CPI (already assembled and sourced in tsx.csv)
cpi = {}
for line in open(os.path.join(HERE, "..", "tsx.csv"), encoding="utf-8"):
    s = line.strip()
    if s and not s.startswith("#"):
        p = [q.strip() for q in s.split(",")]
        cpi[int(p[0])] = float(p[2])
cpi = pd.Series(cpi)

table = pd.DataFrame({"ca_eq": ca_eq, "us_eq": us_eq, "intl_eq": intl_eq,
                      "ca_bonds": ca_bonds, "cash": cash, "cpi": cpi}).loc[FIRST:LAST]
missing = table[table.isna().any(axis=1)]
if len(missing):
    sys.exit(f"missing data:\n{missing}")

HEADER = [l for l in __doc__.split("Sources and splice points")[1].split("\n")[1:] if l.strip()]


def write(fname, header_lines, cols, frame):
    with open(fname, "w", encoding="utf-8", newline="\n") as f:
        for l in header_lines:
            f.write("# " + l.rstrip() + "\n")
        f.write("#\n# " + ",".join(["year"] + cols) + "\n")
        for y, r in frame.iterrows():
            f.write(f"{y}," + ",".join(f"{r[c]:.2f}" for c in cols) + "\n")


cols = ["ca_eq", "us_eq", "intl_eq", "ca_bonds", "cash", "cpi"]
intro = ["Annual asset-class returns in CAD, % (nominal total return), and",
         "Canadian CPI % change. Generated by historical_data/build_returns.py",
         "from the raw files in historical_data/ -- do not edit by hand.", ""]
write(OUT, intro + HEADER, cols, table)
print(f"wrote {os.path.relpath(OUT)}: {FIRST}-{LAST}, {len(table)} years")

args = sys.argv[1:]
if "--blend" in args:
    i = args.index("--blend")
    weights = {k: float(v) for k, v in (kv.split("=") for kv in args[i + 1].split(","))}
    out_name = args[i + 2]
    if abs(sum(weights.values()) - 1) > 1e-9 or not set(weights) <= set(cols[:-1]):
        sys.exit("weights must be among ca_eq,us_eq,intl_eq,ca_bonds,cash and sum to 1")
    blend = sum(table[k] * w for k, w in weights.items())
    out = pd.DataFrame({"return_pct": blend, "cpi_pct": table.cpi})
    desc = ", ".join(f"{k} {w:.0%}" for k, w in weights.items())
    write(out_name,
          [f"Blended portfolio, rebalanced annually: {desc}.",
           "Generated by historical_data/build_returns.py from",
           "historical_returns_cad.csv -- see that file for sources."],
          ["return_pct", "cpi_pct"], out)
    g = (np.prod(1 + blend / 100) ** (1 / len(blend)) - 1) * 100
    print(f"wrote {out_name}: {desc}; annualized {g:.2f}%")

if "--check" in args:
    g = lambda s: (np.prod(1 + s / 100) ** (1 / len(s)) - 1) * 100
    pd.set_option("display.width", 200)
    print("\n== FX: yearly change in JST vs MSCI-implied CAD/USD, 2001-2020")
    f = pd.DataFrame({"jst": jst_ca.xrusd / jst_ca.xrusd.shift(1),
                      "msci": fx_msci / fx_msci.shift(1)}).loc[2001:2020].sub(1).mul(100)
    print(f"   mean abs diff {(f.jst - f.msci).abs().mean():.2f} pts")
    print((f.jst - f.msci).round(2).to_string())
    print("\n== Canadian equities: StatCan TSX TR vs MSCI Canada (CAD), 2000-2015")
    c = pd.DataFrame({"statcan": statcan_tsx, "msci": msci_can}).loc[2000:2015]
    print(f"   mean abs diff {(c.statcan - c.msci).abs().mean():.2f} pts; annualized {g(c.statcan):.2f}% vs {g(c.msci):.2f}%")
    print("\n== US equities: Damodaran vs MSCI USA (both CAD), 1999-2025")
    u = pd.DataFrame({"damodaran": to_cad(sp_usd), "msci": to_cad(annual(usa_usd))}).loc[1999:2025]
    print(f"   mean abs diff {(u.damodaran - u.msci).abs().mean():.2f} pts; annualized {g(u.damodaran):.2f}% vs {g(u.msci):.2f}%")
    print("\n== International: JST stand-in vs MSCI EAFE (both CAD), 1999-2020")
    i_ = pd.DataFrame({"jst": to_cad(jst_intl_usd), "msci": to_cad(annual(eafe_usd))}).loc[1999:2020]
    print(f"   mean abs diff {(i_.jst - i_.msci).abs().mean():.2f} pts; corr {i_.corr().iloc[0, 1]:.3f}; "
          f"annualized {g(i_.jst):.2f}% vs {g(i_.msci):.2f}%")
    print("\n== Cash: T-bill compounded vs JST short rate, 1963-2020")
    k = pd.DataFrame({"tbill": cash_tb, "jst": jst_ca.stir}).loc[1963:2020]
    print(f"   mean abs diff {(k.tbill - k.jst).abs().mean():.2f} pts")
    print("\n== Summary 1957-2025 (annualized %, worst year)")
    for col in cols[:-1]:
        s = table[col]
        print(f"   {col:9s} {g(s):6.2f}%   worst {s.idxmin()} {s.min():6.1f}%   best {s.idxmax()} {s.max():6.1f}%")
    print(f"   cpi       {g(table.cpi):6.2f}%")
