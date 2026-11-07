# Retirement Spending Simulator (C)

A standalone retirement spending simulator for a couple. It models monthly
cash flows, CPP/OAS, per-person tax, registered account withdrawals (with RRIF
minimums and pension-income splitting), TFSA and non-registered accounts,
rent, and one-time events. It reads a configuration file and optionally an
events file, then writes a month-by-month projection of income and balances.

To keep it simple, this program does not include a pre-retirement/income phase.
You will need to use other tools to forecast your savings at retirement. As
such the `retirement_date` configuration is no longer very relevant.

This simulation assumes the government increases CPP/OAS/tax brackets/tax
credits each year by a configurable inflation factor (default 2%), which may
not be true.

Monte Carlo scores are based on provided historical data. A future enhancement
could be to include a skewed Student-t distribution of returns. Aside from
coding, this would require significant user knowledge to set the configurable
properties.

## Build & run

From the project directory:

```
make
```

or

```
cc -std=c11 -Wall -Wextra -O2 -o retirement_sim main.c config.c tax.c rrif.c sim.c monte_carlo.c -lm
```

Then run:

```
./retirement_sim --config sample_config.cfg
```

The default output is `retirement_projection.csv`, and an annual summary is
printed to the console.

Run `./retirement_sim --help` for all options:

| Option | Effect |
|---|---|
| `--config <file>` | Config file (required) |
| `--out <file.csv>` | Output CSV path (default `retirement_projection.csv`) |
| `--roi <pct>` | Override annual investment return |
| `--inflation <pct>` | Override personal (spending) inflation |
| `--gov-inflation <pct>` | Override CPP/OAS/tax-bracket inflation |
| `--target-net <amount>` | Override monthly household net spending target |
| `--target-gross <amount>` | Override monthly per-person gross income target |
| `--years <n>` | Override simulated horizon |
| `--retire-date <YYYY-MM-DD>` | Override retirement date |
| `--tfsa <amount>` | Override starting TFSA balance |
| `--registered-a <amount>` / `--registered-b <amount>` | Override starting registered balances |
| `--monte-carlo` | Run Monte Carlo (bootstrap) using `mc_returns_file` |
| `--mc-in-turn` | Run one Monte Carlo trial per starting year in the returns file |
| `--use-historical-cpi` | In Monte Carlo, use the file's CPI column instead of the fixed inflation rates |

## What the simulator models

The simulator advances month by month. For each month it:

1. **Applies one-time events** scheduled for that month, before anything
   else, so a windfall can fund that month's spending and an expense comes
   out before the month's draws:
   - Positive amounts are split evenly into the two non-registered accounts.
   - Negative amounts come out of non-registered first (proportionally), then
     any remainder from registered accounts, split evenly; if one registered
     account can't cover its half, the other covers the rest.

2. **Computes fixed income** for each person:
   - CPP and OAS, with the standard early/late adjustment factors: CPP early
     0.6% reduction per month, CPP late 0.7% bonus per month, OAS late 0.6%
     bonus per month.
   - Non-registered yield: ROI on the non-registered balance, paid monthly
     and fully taxable. Non-registered principal does not compound.

3. **Draws from each person's registered account** (RRSP/RRIF/LIF) to bring
   their taxable income up to `target_gross_monthly` (a per-person target,
   inflated by personal inflation), never less than the legal RRIF minimum.
   The RRIF minimum is recalculated each January 1 from that account's
   opening balance and age, then spread evenly across the year.

4. **Applies pension-income splitting** (CRA form T1032) to avoid paying more
   tax than necessary when a RRIF minimum forces income above target:
   - If one person's RRIF minimum pushes them over target, the excess is
     reported on the other person's return, and the other person withdraws
     that much less from their own RRIF (never below their own minimum).
   - If both people still end up at or above target, their taxable incomes
     are made equal. The first month this happens is reported at the end of
     the run.
   - No more than 50% of a person's RRIF withdrawal can be transferred, and
     only once that person is 65 or older in the calendar year.
   - Splitting changes whose tax return the income is reported on, not whose
     account the cash comes from.

5. **Sources cash across accounts**: if one person's registered account is
   empty, the other's supplies the cash while the tax attribution stays the
   same.

6. **Computes tax and age credits** separately for each person, using the
   configured progressive tax brackets and the 65+ age credit with its
   income-tested clawback.

7. **Covers any shortfall** against the household net spending target
   (`target_net_monthly`, plus rent once it starts) in this order:
   - non-registered principal, split proportionally across the two accounts;
   - TFSA, up to its balance;
   - an additional taxable registered draw, sized iteratively so it nets the
     needed cash after tax, and limited to what the registered accounts
     actually hold. Its cash comes from each account in proportion to what's
     left, but its tax attribution is rebalanced through income splitting so
     the two incomes stay as equal as the 50% cap allows.

8. **On each January 1**:
   - prints the prior year's closing balances (before any January activity);
   - applies personal and government inflation;
   - recalculates RRIF minimums;
   - adds TFSA contribution room: the annual grant plus the previous year's
     TFSA withdrawals;
   - sweeps non-registered money into the TFSA, up to the available room.

If the household still can't meet its spending target after every source has
been drawn as far as possible, the run reports that money ran out.

## Inflation definitions

The simulator tracks two separate inflation rates:

- `personal_inflation_pct`:
  - Applies to spending targets (`target_net_monthly` and
    `target_gross_monthly`). These raises pause during the optional "go slow"
    window (`go_slow_year` / `go_slow_resume_after_years`).
  - Also increases rent every year (rent ignores the go-slow window).
  - Represents the inflation of household living costs.

- `gov_inflation_pct`:
  - Applies to CPP/OAS benefit amounts, tax brackets, and age-credit
    thresholds.
  - Represents government-indexed benefit and tax inflation.

With `--use-historical-cpi`, Monte Carlo trials replace both rates with the
CPI column from the returns file (rent still uses `personal_inflation_pct`).

## Monte Carlo simulation

Monte Carlo mode replays a historical annual return series to test many
possible return paths.

- `mc_returns_file` must have one line per year in the form
  `year,return_pct,cpi_pct`, with consecutive years. Lines starting with `#`
  and blank lines are ignored. Example:

  ```
  1987,8.02,4.36
  1988,11.80,4.10
  ```

- `--monte-carlo`: each simulated year draws a random year from the file,
  independently and with replacement. `mc_trials` sets the number of trials
  (default 10000) and `mc_seed` makes runs reproducible (0 = seed from the
  clock).
- `--mc-in-turn`: runs one trial per starting year in the file, wrapping
  around if the simulation is longer than the file.

Monte Carlo output includes:
- Success rate: the percentage of trials where money never ran out.
- Ending balance percentiles across trials (p10, p25, median, p75, p90).
- For failed trials, the earliest, median and latest year money ran out.
- A detail CSV, `monte_carlo_results.csv`, with trial-by-trial outcomes.

## Inputs

`sample_config.cfg` defines the simulation assumptions (all values are
placeholders; the sample balances are $0):
- personal data: names, dates of birth, CPP/OAS start dates and amounts at 65.
- account balances: TFSA (combined) and per-person registered and
  non-registered balances, plus TFSA contribution room and annual grant.
- spending targets: `target_net_monthly` (household, after tax) and
  `target_gross_monthly` (per person, before tax).
- ROI, inflation rates, and the go-slow window.
- tax brackets, age-credit rules, rent, the events file, and Monte Carlo
  settings.

### Events file format

The events file is a simple CSV with one event per row:
- `date`: year-month-day when the event occurs
- `amount`: positive for cash inflow, negative for expense
- `note`: a short description of the event

Example rows:

```
2028-06-30,120000,home sale proceeds
2030-09-01,-45000,kitchen renovation
2035-03-15,30000,downsizing relocation cash
```

Events apply at the start of the month they fall in.

## Output

The simulator writes `retirement_projection.csv` with one row per month and
prints an annual summary to the console. In Monte Carlo mode it also writes
`monte_carlo_results.csv`.

Notes on the projection columns:
- `reg_a_wd` / `reg_b_wd` are the registered income reported on each
  person's tax return after income splitting.
- `split_a_to_b` is the amount moved between returns by income splitting:
  positive means part of A's income is reported on B's return, negative the
  reverse.
- Actual cash withdrawn from A's account is `reg_a_wd + split_a_to_b`; from
  B's account it is `reg_b_wd - split_a_to_b`.
- `household_gross` is the couple's combined taxable income and isn't
  affected by splitting.
- `rrif_min_a` / `rrif_min_b` are the monthly RRIF minimums for that year.

## File overview

- `main.c`: CLI parsing and program entry.
- `config.c` / `retirement_sim.h`: config parsing, defaults, events loading,
  CLI overrides, and data structures.
- `sim.c`: monthly simulation engine and output.
- `tax.c`: progressive tax calculation and age-credit logic.
- `rrif.c`: CRA RRIF minimum withdrawal factors.
- `monte_carlo.c`: Monte Carlo trial runner.
- `dateutil.h`: date utilities.
- `CLAUDE.md`: project context and working notes for Claude agents.
- `sample_config.cfg`: example configuration file to edit for your case.
- `sample_events.csv`: example one-time events schedule.

Return and inflation data files. The first four are in the
`year,return_pct,cpi_pct` format Monte Carlo expects, and all share the same
Canadian CPI for the years they overlap:
- `perplexity-40-year.csv` (1987–2025, the sample config's default):
  Perplexity-generated blended series (MSCI EAFE, TSX and S&P 500 at 20%
  each, plus 0.8% for fixed income) with Canadian CPI.
- `proxy-30-year.csv` (1996–2025): Perplexity-generated estimate of returns
  for a 10/60/30 portfolio based on weighted averages.
- `sp-30-year.csv` (1996–2025): Perplexity-generated S&P 500 returns.
- `tsx.csv` (1947–2025): Perplexity-generated TSX returns.
- `canada_inflation_1986_2025.csv`: Canadian annual inflation only (no
  returns), 1986–2025; reference data, not loadable by Monte Carlo.
- `example_returns.csv`: placeholder values only, not real data.

CPI sources: 1987–2025 as in `perplexity-40-year.csv`; 1960–1986 from the
World Bank series derived from Statistics Canada's CPI; 1947–1959 from
Statistics Canada's published annual average CPI change. Each file's header
comments repeat this.

## Getting started

1. Copy `sample_config.cfg` to a real config file and enter your actual
   values.
2. Adjust the tax brackets and age credit values to your jurisdiction.
3. Run the simulator with `--config` and review the generated CSV.
4. To run Monte Carlo, point `mc_returns_file` at a file in the
   `year,return_pct,cpi_pct` format (the sample config already uses
   `perplexity-40-year.csv`) and add `--monte-carlo` or `--mc-in-turn`.
