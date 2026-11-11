#!/usr/bin/env python3
"""Regression tests for the retirement simulator.

Run from the repo root with `make test` (builds first) or
`python3 tests/run_tests.py`. Standard library only.

Each test writes a small, synthetic config into a temporary directory, runs
the simulator, and checks the output. Wherever possible the expected value is
computed here, independently, from the rule being tested (CRA/Service Canada
rules as documented in CLAUDE.md) -- not copied from a previous run -- so a
test catches a wrong rule, not just a change in output.
"""
import csv
import math
import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, "retirement_sim")
TMP = tempfile.mkdtemp(prefix="retsim-tests-")

results = []


# ------------------------------------------------------------------ helpers
BRACKETS = [(0, 0.0), (16452, 0.20), (58523, 0.30), (117045, 0.40)]

BASE = {
    "person_a_name": "A", "person_b_name": "B",
    "person_a_dob": "1955-01-01", "person_b_dob": "1955-01-01",
    "person_a_cpp_start": "2020-01-01", "person_b_cpp_start": "2020-01-01",
    "person_a_oas_start": "2020-01-01", "person_b_oas_start": "2020-01-01",
    "person_a_cpp_at_65": "1400", "person_a_oas_at_65": "740",
    "person_b_cpp_at_65": "1400", "person_b_oas_at_65": "740",
    "retirement_date": "2020-01-01", "sim_start_date": "2027-01-01", "sim_years": "2",
    "tfsa_balance": "0",
    "person_a_registered_balance": "500000", "person_b_registered_balance": "500000",
    "person_a_nonreg_balance": "0", "person_b_nonreg_balance": "0",
    "roi_annual_pct": "5.0", "personal_inflation_pct": "2.0", "gov_inflation_pct": "2.0",
    "target_net_monthly": "6000", "target_gross_monthly": "4583",
}


def write_cfg(name, overrides=None, extra_lines=()):
    cfg = dict(BASE)
    cfg.update(overrides or {})
    path = os.path.join(TMP, name + ".cfg")
    with open(path, "w") as f:
        for k, v in cfg.items():
            if v is not None:
                f.write(f"{k}={v}\n")
        for lo, rt in BRACKETS:
            f.write(f"tax_bracket={lo},{rt}\n")
        for line in extra_lines:
            f.write(line + "\n")
    return path


def run(cfg_path, name, *args):
    """Run the simulator; return (stdout+stderr, rows as dicts keyed by date)."""
    out_csv = os.path.join(TMP, name + ".csv")
    p = subprocess.run([BIN, "--config", cfg_path, "--out", out_csv, *args],
                       capture_output=True, text=True, cwd=TMP)
    rows = {}
    if os.path.exists(out_csv) and "--monte-carlo" not in args and "--mc-in-turn" not in args:
        with open(out_csv) as f:
            for r in csv.DictReader(f):
                rows[r["date"]] = {k.strip(): v for k, v in r.items()}
    return p.stdout + p.stderr, rows


def f(rows, date, key):
    return float(rows[date][key])


def tax(annual, gi=1.0, brackets=BRACKETS):
    t = 0.0
    for i, (lo, rt) in enumerate(brackets):
        lo *= gi
        hi = brackets[i + 1][0] * gi if i + 1 < len(brackets) else 1e18
        if annual > lo:
            t += (min(annual, hi) - lo) * rt
    return t


def check(name, cond, detail=""):
    results.append((name, bool(cond), detail))


def close(a, b, tol=0.015):
    return abs(a - b) <= tol


# -------------------------------------------------------------------- tests
def test_oas_clawback():
    # Capped: income far above the threshold -> clawback = full OAS.
    cfg = write_cfg("claw_cap", {"person_a_registered_balance": "3000000",
                                 "person_b_registered_balance": "3000000"})
    _, rows = run(cfg, "claw_cap")
    r = "2027-01-01"
    inc = f(rows, r, "reg_a_wd") + 1400 + 740
    exp = min(740, 0.15 * (inc * 12 - 95323) / 12)
    check("OAS clawback, capped at full OAS", close(f(rows, r, "oas_clawback_a"), exp),
          f"expected {exp:.2f}, got {f(rows, r, 'oas_clawback_a'):.2f}")
    exp_tax = -tax((inc - exp) * 12) / 12
    check("OAS clawback is not taxed (tax on income - clawback)", close(f(rows, r, "tax_a"), exp_tax),
          f"expected {exp_tax:.2f}, got {f(rows, r, 'tax_a'):.2f}")
    check("OAS clawback cap indexes with gov inflation",
          close(f(rows, "2028-01-01", "oas_clawback_a"), 740 * 1.02),
          f"expected {740 * 1.02:.2f}, got {f(rows, '2028-01-01', 'oas_clawback_a'):.2f}")
    # Partial clawback.
    cfg = write_cfg("claw_part", {"person_a_registered_balance": "1500000",
                                  "person_b_registered_balance": "1500000"})
    _, rows = run(cfg, "claw_part")
    inc = f(rows, r, "reg_a_wd") + 1400 + 740
    exp = min(740, 0.15 * (inc * 12 - 95323) / 12)
    check("OAS clawback, partial", 0 < exp < 740 and close(f(rows, r, "oas_clawback_a"), exp),
          f"income {inc * 12:.0f}/yr: expected {exp:.2f}, got {f(rows, r, 'oas_clawback_a'):.2f}")


def test_survivor():
    ov = {"person_a_dob": "1958-01-01", "person_b_dob": "1958-01-01",
          "person_a_cpp_start": "2023-01-01", "person_b_cpp_start": "2023-01-01",
          "person_a_oas_start": "2023-01-01", "person_b_oas_start": "2023-01-01",
          "person_b_cpp_at_65": "900", "sim_years": "6", "tfsa_balance": "100000",
          "person_b_registered_balance": "300000", "person_a_death_date": "2028-03-10"}
    cfg = write_cfg("survivor", ov)
    out, rows = run(cfg, "survivor")
    m, a = "2028-03-01", "2028-04-01"
    check("Death applies from the next month", rows[a]["event"].startswith("Death of A")
          and f(rows, m, "registered_balance_a") > 0, rows[a]["event"])
    check("Deceased has no draw, tax or credit after death",
          f(rows, a, "reg_a_wd") == 0 and abs(f(rows, a, "tax_a")) < 0.005 and f(rows, a, "registered_balance_a") == 0)
    gi = 1.02
    cpp_b = min(900 + 0.60 * 1400, 1531.56) * gi          # own + 60%, capped at combined max
    oas_b = 740 * gi
    nonreg_start = f(rows, m, "non-registered_balance_a") + f(rows, m, "non-registered_balance_b") + 2500
    inc = f(rows, a, "reg_b_wd") + cpp_b + oas_b + nonreg_start * 0.05 / 12
    check("Survivor CPP = own + 60% of deceased, capped at the combined maximum",
          close(f(rows, a, "household_gross"), inc, 0.03),
          f"expected gross {inc:.2f}, got {f(rows, a, 'household_gross'):.2f}")
    exp_tax = -tax(inc * 12, gi) / 12
    check("Survivor taxed on one return", close(f(rows, a, "tax_b"), exp_tax, 0.03),
          f"expected {exp_tax:.2f}, got {f(rows, a, 'tax_b'):.2f}")
    check("Survivor spending = 80% of target", close(f(rows, a, "net_takehome"), 6000 * gi * 0.8),
          f"expected {6000 * gi * 0.8:.2f}, got {f(rows, a, 'net_takehome'):.2f}")
    check("Deceased's RRIF minimum carries to the survivor for the year",
          close(f(rows, a, "rrif_min_b"), f(rows, m, "rrif_min_a") + f(rows, m, "rrif_min_b")))
    # Second death ends the run.
    cfg2 = write_cfg("survivor2", dict(ov, person_b_death_date="2031-07-01"))
    out2, _ = run(cfg2, "survivor2")
    check("Second death ends the simulation and reports the estate",
          "ended at the second death (2031-07)" in out2)
    # --death-a on the command line == the same date in the config.
    cfg3 = write_cfg("survivor3", {k: v for k, v in ov.items() if k != "person_a_death_date"})
    _, rows3 = run(cfg3, "survivor3", "--death-a", "2028-03-10")
    check("--death-a gives the same result as the config date", rows3 == rows)


def test_event_funding():
    # TFSA first, then a grossed-up RRIF lump taxed on the year's income.
    cfg = write_cfg("event", {"tfsa_balance": "20000", "events_file": os.path.join(TMP, "ev.csv"),
                              "target_net_monthly": "4000", "target_gross_monthly": "4583"})
    with open(os.path.join(TMP, "ev.csv"), "w") as fh:
        fh.write("2027-01-01,-50000,expense\n")
    _, rows = run(cfg, "event")
    r = "2027-01-01"
    tfsa, gross, tx = (f(rows, r, k) for k in ("event_from_tfsa", "event_from_registered", "event_tax"))
    check("Expense uses the TFSA before the RRIFs", close(tfsa, 20000), f"TFSA {tfsa:.2f}")
    check("RRIF lump is grossed up: gross - tax = remainder", close(gross - tx, 30000, 0.02),
          f"gross {gross:.2f} - tax {tx:.2f} = {gross - tx:.2f}")
    inc = (f(rows, r, "reg_a_wd") + 1400 + 740) * 12      # identical spouses: lump splits evenly

    def cost(x):
        claw = min(740 * 12, max(0.0, 0.15 * (x - 95323)))
        return tax(x - claw) + claw
    exp = 2 * (cost(inc + gross / 2) - cost(inc))
    check("Lump tax = extra tax on the YEAR's income", close(tx, exp, 0.05),
          f"expected {exp:.2f}, got {tx:.2f}")


def test_rrif_age():
    # Minimum factor uses age on January 1 (CRA), not the age reached that year.
    cfg = write_cfg("rrif", {"person_a_dob": "1958-05-23", "person_b_dob": "1958-05-23",
                             "person_a_cpp_start": "2023-06-01", "person_b_cpp_start": "2023-06-01",
                             "person_a_oas_start": "2023-06-01", "person_b_oas_start": "2023-06-01",
                             "sim_years": "8", "target_gross_monthly": "100"})
    _, rows = run(cfg, "rrif")
    for year, age, factor in [(2028, 69, 1 / 21), (2034, 75, 0.0582)]:
        got = f(rows, f"{year}-01-01", "rrif_min_a") * 12 / f(rows, f"{year - 1}-12-01", "registered_balance_a")
        check(f"RRIF factor at Jan 1 age {age} = CRA {factor:.4f}", close(got, factor, 0.00006),
              f"got {got:.4f}")


def test_dollars_year():
    # dollars_year=2026 with a 2027 start == every government amount x1.02 by hand.
    cfg = write_cfg("dy", {"dollars_year": "2026", "sim_years": "5"},
                    ["fed_age_credit_amount=9208", "fed_age_credit_rate=0.14",
                     "fed_age_credit_clawback_start=46432", "prov_age_credit_amount=6342",
                     "prov_age_credit_rate=0.0505", "prov_age_credit_clawback_start=47210"])
    _, a = run(cfg, "dy")
    g = 1.02
    global BRACKETS
    saved = BRACKETS
    BRACKETS = [(lo * g, rt) for lo, rt in saved]
    cfg2 = write_cfg("dy_manual", {"sim_years": "5",
                                   "person_a_cpp_at_65": repr(1400 * g), "person_a_oas_at_65": repr(740 * g),
                                   "person_b_cpp_at_65": repr(1400 * g), "person_b_oas_at_65": repr(740 * g)},
                     [f"fed_age_credit_amount={9208 * g!r}", "fed_age_credit_rate=0.14",
                      f"fed_age_credit_clawback_start={46432 * g!r}", f"prov_age_credit_amount={6342 * g!r}",
                      "prov_age_credit_rate=0.0505", f"prov_age_credit_clawback_start={47210 * g!r}",
                      f"oas_clawback_threshold={95323 * g!r}", f"cpp_combined_max_monthly={1531.56 * g!r}"])
    BRACKETS = saved
    _, b = run(cfg2, "dy_manual")
    diff = max(abs(float(a[d][k]) - float(b[d][k])) for d in a for k in a[d] if k not in ("date", "event"))
    check("dollars_year == indexing every government amount by hand", diff < 0.011, f"max diff {diff:.4f}")


def test_surplus():
    # Draws to the gross target exceed a low net target: the surplus is saved.
    cfg = write_cfg("surplus", {"target_net_monthly": "3000"})
    _, rows = run(cfg, "surplus")
    r = "2027-01-01"
    check("Surplus month: take-home equals the target", close(f(rows, r, "net_takehome"), 3000),
          f"got {f(rows, r, 'net_takehome'):.2f}")
    check("Surplus is saved to non-registered (negative shortfall draw)",
          f(rows, r, "non_reg_shortfall_draw") < 0
          and f(rows, r, "non-registered_balance_a") + f(rows, r, "non-registered_balance_b") > 0)


def test_depletion_note():
    cfg = write_cfg("deplete", {"person_a_registered_balance": "50000", "person_b_registered_balance": "50000",
                                "sim_years": "5"})
    out, _ = run(cfg, "deplete")
    check("Exhausted accounts are reported (not 'No account was depleted')",
          "registered account was exhausted" in out and "No account was depleted" not in out)


def test_monte_carlo():
    cfg_base = {"sim_years": "10", "mc_trials": "300", "mc_seed": "7", "target_net_monthly": "5000"}
    # Loader: >128 rows (old realloc bug) and consecutive years.
    big = os.path.join(TMP, "big.csv")
    with open(big, "w") as fh:
        fh.write("# year,return_pct,cpi_pct\n")
        for y in range(1700, 2000):
            fh.write(f"{y},{5 + (y % 7) - 3},2.0\n")
    out, _ = run(write_cfg("mc_big", dict(cfg_base, mc_returns_file=big)), "mc_big", "--monte-carlo")
    check("Returns file with 300 rows loads", "has 300 usable years" in out)
    # Asset-class weights == an independently blended single-return file.
    hist = os.path.join(REPO, "historical_returns_cad.csv")
    w = [0.2, 0.2, 0.2, 0.3, 0.1]
    blend = os.path.join(TMP, "blend.csv")
    with open(hist) as src, open(blend, "w") as dst:
        dst.write("# year,return_pct,cpi_pct\n")
        for line in src:
            if line.strip() and not line.startswith("#"):
                v = [float(x) for x in line.split(",")]
                dst.write(f"{int(v[0])},{sum(w[i] * v[i + 1] for i in range(5)):.12f},{v[6]}\n")
    alloc = [f"mc_alloc_{n}={x}" for n, x in zip(["ca_eq", "us_eq", "intl_eq", "ca_bonds", "cash"], w)]
    o1, _ = run(write_cfg("mc_alloc", dict(cfg_base, mc_returns_file=hist), alloc), "mc_alloc",
                "--monte-carlo", "--use-historical-cpi")
    o2, _ = run(write_cfg("mc_blend", dict(cfg_base, mc_returns_file=blend)), "mc_blend",
                "--monte-carlo", "--use-historical-cpi")
    pick = lambda o: [l for l in o.splitlines() if l.startswith("Success rate") or "p50" in l]
    check("mc_alloc_* weights == the same blend precomputed", pick(o1) and pick(o1) == pick(o2),
          f"{pick(o1)} vs {pick(o2)}")
    # mc_real_return_* re-centres each asset exactly on its target.
    o3, _ = run(write_cfg("mc_real", dict(cfg_base, mc_returns_file=hist),
                          alloc + ["mc_real_return_ca_eq=4.1"]), "mc_real", "--monte-carlo", "--use-historical-cpi")
    rows = [[float(x) for x in l.split(",")] for l in open(hist) if l.strip() and not l.startswith("#")]
    g = math.exp(sum(math.log((1 + r[1] / 100) / (1 + r[6] / 100)) for r in rows) / len(rows))
    k = 1.041 / g
    check("mc_real_return_ca_eq multiplier = (1+target)/historical real",
          f"x {k:.4f})" in o3, f"expected multiplier {k:.4f}")
    # Guardrails: a trial identical to the flat plan never cuts.
    flat = os.path.join(TMP, "flat.csv")
    with open(flat, "w") as fh:
        fh.write("# year,return_pct,cpi_pct\n")
        for y in range(1900, 2000):
            fh.write(f"{y},5.0,2.0\n")
    o4, _ = run(write_cfg("gr_flat", dict(cfg_base, mc_returns_file=flat)), "gr_flat",
                "--mc-in-turn", "--use-historical-cpi", "--with-guardrails")
    check("Guardrails: a trial on the plan's path never cuts", "Never cut spending:        100.0%" in o4,
          next((l for l in o4.splitlines() if "Never cut" in l), "no guardrail table"))
    # Guardrails: a crash steps the cut down to the 20% cap.
    crash = os.path.join(TMP, "crash.csv")
    with open(crash, "w") as fh:
        fh.write("# year,return_pct,cpi_pct\n")
        for i in range(100):
            fh.write(f"{1900 + i},{-25.0 if 1 <= i <= 3 else 5.0},2.0\n")
    run(write_cfg("gr_crash", dict(cfg_base, mc_returns_file=crash, sim_years="15")), "gr_crash",
        "--mc-in-turn", "--use-historical-cpi", "--with-guardrails")
    with open(os.path.join(TMP, "monte_carlo_results.csv")) as fh:
        t1 = next(csv.DictReader(fh))
    # The crash is in the 2nd simulated year (2028); the first cut is the following January.
    check("Guardrails: a crash cuts in steps to the 20% cap, starting the next January",
          float(t1["guardrail_max_cut_pct"]) == 20.0 and t1["guardrail_first_cut_year"] == "2029",
          f"deepest {t1['guardrail_max_cut_pct']}%, first cut {t1['guardrail_first_cut_year']}")


# --------------------------------------------------------------------- main
def main():
    if not os.path.exists(BIN):
        sys.exit("retirement_sim not built; run `make test` (or `make` first).")
    for t in (test_oas_clawback, test_survivor, test_event_funding, test_rrif_age, test_dollars_year,
              test_surplus, test_depletion_note, test_monte_carlo):
        try:
            t()
        except Exception as e:  # a crash in one test shouldn't hide the others
            results.append((t.__name__, False, f"exception: {e!r}"))
    width = max(len(n) for n, _, _ in results)
    for name, ok, detail in results:
        print(f"{'PASS' if ok else 'FAIL'}  {name:{width}}  {'' if ok else detail}")
    failed = sum(1 for _, ok, _ in results if not ok)
    print(f"\n{len(results) - failed}/{len(results)} passed  (work files in {TMP})")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
