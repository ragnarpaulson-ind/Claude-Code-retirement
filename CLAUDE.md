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
  `-std=c11 -Wall -Wextra`. Run the program against a config and inspect the
  CSV before handing work back.
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
   two non-registered accounts. Negative events come out of non-registered
   first (proportionally), then any remainder from registered, split evenly;
   if one registered account can't cover its half, the other covers the rest.
2. **Fixed income per person:** CPP/OAS (early/late adjustment factors,
   indexed by gov inflation) plus non-registered yield (ROI × balance,
   monthly, taxable). Non-registered principal does not compound.
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
- Warnings: "registered funds insufficient for base/RRIF-minimum draw" and
  "money ran out" (every source exhausted, target still unmet).

## Monte Carlo
- `mc_returns_file` lines: `year,return_pct,cpi_pct`; years must be
  consecutive; `#` comments and blank lines ignored.
- `--monte-carlo`: each simulated year drawn independently with replacement.
  `--mc-in-turn`: one trial per starting year, wrapping around.
- Config keys: `mc_trials` (default 10000), `mc_returns_file`, `mc_seed`
  (0 = time-seeded). Writes `monte_carlo_results.csv`.
- Return data: `historical_returns_cad.csv` (1957-2025, CAD, by asset class)
  is generated by `historical_data/build_returns.py` from the raw source
  files in `historical_data/` -- regenerate, never hand-edit. Blend it with
  `--blend` to get a 3-column file the loader reads. The older
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

## Open items / known issues
1. Non-registered yield is computed on the live balance, so registered draws
   drift month to month within a year while non-reg is being drawn down.
   Option discussed: compute yield from the January 1 balance and hold it
   flat for the year (not implemented).
2. Non-registered principal does not compound; ROI above needed income is
   not handled (owner is aware, deferred).
3. Monte Carlo reads a pre-blended 3-column file. Planned: read
   `historical_returns_cad.csv` directly and blend by allocation weights set
   in the config (e.g. `alloc_ca_eq=0.20`), rebalanced annually.
4. Planned next: OAS recovery tax (clawback), then survivor modelling
   (death date, ~80% spending, survivor CPP, single-return tax).
