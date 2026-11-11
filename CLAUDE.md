# CLAUDE.md — Retirement Spending Simulator (C)

Guidance for Claude agents working in this repo. Read this before changing code.

## What this is
A C rewrite of the owner's retirement spending spreadsheet: a rules-based,
config-driven, month-by-month simulation for a retired couple (Person A and
Person B). It models CPP/OAS, per-person progressive tax with the 65+ age
credit, RRIF minimums, TFSA and non-registered accounts, one-time events,
rent, and Monte Carlo runs over historical returns. There is no
pre-retirement/accumulation phase.

New inputs should be added as config-file keys (key=value) rather than
hard-coded, wherever possible.

## Working style (owner's preferences)
- Iterate ONE change at a time: propose, implement that one change, let the
  owner test, then move on.
- Confirm design decisions that could reasonably go either way before
  patching.
- Build with the Makefile (`make`); it must compile with no warnings under
  `-std=c11 -Wall -Wextra`. Run `make test` (all must pass) and run the
  program against a config and inspect the CSV before handing work back.
- When fixing a bug or adding a rule, add a test to `tests/run_tests.py`
  whose expected value is computed independently from the rule, not copied
  from the program's output.
- Owner's planning choices -- do not "fix" these without asking:
  - The OAS 10% increase at 75 is deliberately NOT modelled (future
    benefits may be cut); leave it out as a built-in margin.
  - Death dates are fixed scenarios, not random mortality.
- The owner's real config and events files (e.g. `retire.cfg`,
  `bigevents.csv`) live in their local working copy and are not committed.
  `sample_config.cfg` / `sample_events.csv` are placeholders (sample balances
  are $0, so "money ran out" warnings there are expected).

## Build & run
```
make
./retirement_sim --config sample_config.cfg [--out file.csv]
./retirement_sim --config sample_config.cfg --monte-carlo [--use-historical-cpi]
./retirement_sim --config sample_config.cfg --mc-in-turn
./retirement_sim --help
make test        # regression tests (tests/run_tests.py, Python stdlib only)
```

## Files
- `main.c` — CLI parsing, chooses single run vs Monte Carlo.
- `config.c` / `retirement_sim.h` — defaults, config/events parsing, CLI
  overrides, data structures.
- `sim.c` — the monthly engine (`run_simulation_ex`), CSV output, summary.
- `tax.c` — bracket-table tax and age credit (credit floored at zero).
- `rrif.c` — CRA RRIF minimum factors.
- `monte_carlo.c` — bootstrap and in-turn Monte Carlo over a historical file.
- `dateutil.h` — date helpers.
- `*.csv` return/inflation series — see README; `example_returns.csv` is a
  placeholder, not real data.

## Model rules (sim.c, each month)
1. **Start of month: one-time events.** Positive events split evenly into the
   two non-registered accounts. Negative events: non-registered first
   (proportionally), then TFSA (room restored next Jan), then the rest is a
   registered lump settled after the month's regular draws: grossed up so
   gross − tax = remainder, tax = annual_tax_cost(12×regular income + lump)
   − annual_tax_cost(12×regular income) per person (tax − age credit + OAS
   clawback), split by remaining registered balance, no pension splitting.
   Unmet remainder counts as money ran out. CSV: event_from_tfsa,
   event_from_registered, event_tax.
2. **Fixed income per person:** CPP/OAS (early/late adjustment factors,
   indexed by gov inflation) plus non-registered yield (net ROI × balance,
   monthly, taxable). Non-registered principal does not compound.
   Net ROI = gross return (`roi_annual_pct`, or the Monte Carlo year's
   return) minus `annual_fee_pct`; it applies to every pool.
3. **Base registered draw per person:** `need` = draw that lands taxable
   income exactly on `target_gross_monthly` (per person, personal-inflated);
   floored at that person's RRIF minimum.
4. **Pension-income splitting (CRA T1032).** `split_a_to_b` is tax
   attribution only (+ = A's income on B's return).
   - Absorb: if one person's RRIF minimum pushes them over target, the excess
     is attributed to the spouse and the spouse's own withdrawal is cut by
     the same amount (never below the spouse's RRIF minimum).
   - Equalize: if one is still over and the other is at/over target, taxable
     incomes are equalized (tax only, no cash change); first occurrence is
     reported in the end-of-run summary.
   - Cap: total transferred ≤ 50% of the transferor's RRIF withdrawal, and
     only once the transferor is 65+ in that calendar year.
5. **Cross-account cash sourcing:** if one registered account is dry, the
   other supplies the cash; tax attribution is unchanged.
   Tax, per person (compute_net) -- OAS clawback: claw = min(OAS, oas_clawback_rate ×
   (annual income − oas_clawback_threshold × gov_inflate)) per person,
   applied same-year; taxable income and the age-credit test use
   income − claw, and net_of_tax subtracts it. CSV columns
   `oas_clawback_a/b` sit before `event`.
6. **Shortfall vs net target** (`target_net_monthly`, personal-inflated, plus
   rent once `rent_start_date` passes) is covered in order: non-registered
   principal → TFSA → extra taxable registered draw, solved by iterative
   gross-up and capped at real leftover registered capacity. The extra draw's
   cash comes from each account in proportion to leftover capacity, but its
   tax attribution is rebalanced through the split so incomes stay equal
   (within the 50% cap).
7. **January 1 only:** print prior-year close (before any January activity),
   apply inflation, re-baseline RRIF minimums from opening balance and age,
   add TFSA room (annual grant + prior-year TFSA withdrawals), then sweep
   non-registered into TFSA up to available room.

8. **Deaths** (`person_x_death_date`, at the start of the first month on or
   after it, before events): first death rolls RRIF, non-reg and the rest
   of the year's RRIF minimum to the survivor, adds `cpp_death_benefit` to
   the survivor's non-reg; afterwards the deceased has no income/draw/split
   (`both_alive` gates splitting), survivor CPP = own + min(cpp_survivor_rate
   × deceased CPP, combined max − own), net target × survivor_spending_pct
   (rent added after, in full), positive events go to the survivor. Second
   death: break out of the month loop; estate noted in the summary.
   CLI `--death-a/--death-b <date|none>` override the config dates.

`dollars_year` (0 = start year): gov_inflate starts at
(1 + gov_inflation_pct)^(start year − dollars_year) instead of 1, so every
government amount entered in that year's dollars is brought forward.
Inflation: `personal_inflation_pct` for spending targets (paused during the
"go slow" window) and rent (always inflates, ignores go-slow);
`gov_inflation_pct` for CPP/OAS, tax brackets and credits. With
`--use-historical-cpi` in Monte Carlo, the historical CPI replaces both
(except rent, which still uses `personal_inflation_pct`).

## CSV semantics worth knowing
- `reg_a_wd` / `reg_b_wd` are registered income ATTRIBUTED to each tax
  return after splitting. Cash actually withdrawn from A's account is
  `reg_a_wd + split_a_to_b`; from B's is `reg_b_wd − split_a_to_b`.
- `household_gross` is unaffected by splitting.
- Surplus months (after-tax income > target): saved into non-reg (by
  balance, or split evenly between the living spouses if both are 0);
  `non_reg_shortfall_draw` is negative and `net_takehome` = actual spending.
- Warnings: "registered funds insufficient for base/RRIF-minimum draw" and
  "money ran out" (every source exhausted, target still unmet).

## Monte Carlo
- `mc_returns_file`: one line per consecutive year; `#` comments and blank
  lines ignored, except a `# year,...` comment that names the columns.
  With `mc_alloc_<ca_eq|us_eq|intl_eq|ca_bonds|cash>` weights (fractions
  summing to 1) the loader blends those named columns each year
  (rebalanced annually) and takes CPI from the `cpi` column; without
  weights it expects `year,return_pct,cpi_pct`.
- Optional `mc_real_return_<asset>` (% real per year) re-centres that
  asset before blending: every year's (1+r) is multiplied by
  k = (1+target) / (compound real return over the file), so the compound
  real average equals the target and each year keeps its CPI and relative
  deviation. Needs `mc_alloc_*` weights; meant for `--use-historical-cpi`.
- `--with-guardrails`: run_monte_carlo first runs the flat plan (roi_override
  NULL) and passes its `year_end_real_total[]` path (Dec 31 total balance /
  cumulative CPI, no go-slow pause) to each trial via run_simulation_ex's
  `plan` argument. Each January: behind plan by > guardrail_trigger_pct ->
  cut += guardrail_step_pct (cap guardrail_max_cut_pct); ratio >= 1 ->
  cut = 0; else hold. spend_scale = 1 - cut scales target_net_effective
  (incl. rent) and target_gross_pp. If the plan's balance is ~0 there is no
  path: hold.
- `--monte-carlo`: each simulated year drawn independently with replacement.
  `--mc-in-turn`: one trial per starting year, wrapping around.
- Config keys: `mc_trials` (default 10000), `mc_returns_file`, `mc_seed`
  (0 = time-seeded). Writes `monte_carlo_results.csv`.
- Return data: `historical_returns_cad.csv` (1957-2025, CAD, by asset class)
  is generated by `historical_data/build_returns.py` from the raw source
  files in `historical_data/` -- regenerate, never hand-edit. The sample
  config reads it directly with `mc_alloc_*` weights. The older
  `perplexity-40-year.csv`, `proxy-30-year.csv`, `sp-30-year.csv` and
  `tsx.csv` are LLM-generated with known errors (see README).

## Fixed bugs — do not reintroduce
- Year-close print used post-January-sweep balances (must print before
  January activity).
- `net_takehome` omitted the non-registered shortfall draw (false "money ran
  out" warnings).
- Shortfall was computed against phantom registered income before checking
  real cash availability.
- First/last year-close lines print even when partial, labelled
  "(partial, N mo)".
- One-time events were applied after the month's draws, causing a spurious
  taxable RRIF spike when an event coincided with a spending jump.
- The extra registered draw was attributed by account size, not split.
- RRIF minimums used the age reached during the year (year - dob.year);
  CRA uses age on Jan 1 -- see age_on_jan1().
- Expenses beyond non-reg funds were taken from the RRIFs untaxed, skipping
  the TFSA.
- A surplus with both non-reg balances at 0 was silently counted as spent
  (and net_takehome showed income, not spending, in surplus months).
- Account-exhausted notes only fired on a negative balance; draws are capped
  so balances land on 0, and the summary wrongly said "No account was
  depleted". Now flagged when an account reaches ~0 from a positive balance.

## Open items / known issues
1. Non-registered yield is computed on the live balance, so registered draws
   drift month to month within a year while non-reg is being drawn down.
   Option discussed: compute yield from the January 1 balance and hold it
   flat for the year (not implemented).
2. Non-registered principal does not compound; ROI above needed income is
   not handled (owner is aware, deferred).
3. Block bootstrap dropped (owner's call: --mc-in-turn and independent
   years bracket it). Shortfall reporting folded into guardrail stats.
4. Survivor modelling done (fixed death dates). Not modelled: random
   mortality in Monte Carlo (owner's choice); CPP survivor rules under 65;
   OAS +10% at 75 (owner's choice); GIS.
5. Independent review (Nov 2026) open findings: Ontario Health Premium and
   the pension income credit not modelled; non-reg return
   taxed as 100% income; TFSA grant not indexed or
   halved after a death; cross-account sourcing can attribute > 50%;
   Monte Carlo with mc_alloc_* but no --use-historical-cpi gives no warning;
   tax is monthly income x 12 (uneven years over-taxed); event lumps are
   not pension-split.
