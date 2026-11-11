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
| `--death-a <YYYY-MM-DD\|none>` / `--death-b ...` | Survivor scenario: override the death date in the config (`none` ignores one set there); Monte Carlo uses it in every trial |
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
   - **OAS clawback (recovery tax):** each person repays `oas_clawback_rate`
     (15%) of their net income above `oas_clawback_threshold` ($95,323 for
     2026, indexed with government inflation), up to their full OAS. The
     repayment is deductible, so clawed-back OAS is not also taxed, and the
     age credit is tested on income after it. CRA actually recovers it from
     the following July–June OAS payments; the simulator applies it in the
     same year. The extra registered draw is sized with the clawback
     included.

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

9. **Survivor modelling** (when `person_a_death_date` and/or
   `person_b_death_date` is set), from the first month starting on or after
   the death date:
   - the deceased's RRIF and non-registered balances roll over tax-free to
     the survivor (the TFSA is already combined), with the rest of that
     year's RRIF minimum; from the next January the minimum uses the
     survivor's age;
   - the deceased's CPP and OAS stop; the survivor gets the CPP survivor's
     pension, `cpp_survivor_rate` (60%, the 65+ rule) of the deceased's
     CPP, with own + survivor's pension capped at `cpp_combined_max_monthly`
     ($1,531.56 in 2026, indexed); the CPP death benefit
     (`cpp_death_benefit`, $2,500) is paid once into the survivor's
     non-registered account;
   - tax is on the survivor's return alone, with no pension splitting
     (where the OAS clawback can apply);
   - the net spending target drops to `survivor_spending_pct` (80%) of the
     couple's, while rent continues in full; the per-person gross target is
     unchanged;
   - later one-time events go entirely to the survivor;
   - if both deaths fall within the horizon, the simulation ends at the
     second and reports the estate (nominal and in today's dollars).
   Death dates are fixed: every Monte Carlo trial (and the guardrails' flat
   plan) uses the same dates. Set them in the config or, to try scenarios
   quickly, with `--death-a` / `--death-b` on the command line, e.g.
   `--death-a 2036-05-23`; the run header confirms the scenario in use. The `event` column shows "Death of <name>".

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

- `mc_returns_file` has one line per year, with consecutive years. Lines
  starting with `#` and blank lines are ignored, except a `# year,...`
  comment, which names the columns. Two layouts are accepted:
  - **Asset classes** (e.g. `historical_returns_cad.csv`): columns named
    `ca_eq`, `us_eq`, `intl_eq`, `ca_bonds`, `cash` and `cpi`, blended by the
    `mc_alloc_<name>` weights in the config (see "Historical returns" below).
  - **Single return**, when no `mc_alloc_*` weights are set:
    `year,return_pct,cpi_pct`, e.g.

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
- `annual_fee_pct`: investment fees (e.g. ETF MERs), deducted from every
  year's return in both flat and Monte Carlo runs (default 0; leave it at 0
  if `roi_annual_pct` is already net of fees). The historical return files
  are index returns before fees, so set this for Monte Carlo.
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
- `non_reg_shortfall_draw` is negative in a month with a **surplus**
  (after-tax income above the spending target, e.g. RRIF minimums or the
  gross-target draws netting more than you spend): the surplus is saved to
  the non-registered accounts, and January's sweep moves it into the TFSA
  when there is room. `net_takehome` is what was actually spent.
- `oas_clawback_a` / `oas_clawback_b` are the monthly OAS recovery tax for
  each person (already taken out of `net_of_tax`).
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

### Historical returns (recommended)

`historical_returns_cad.csv` holds annual returns for 1957–2025 by asset
class, all in Canadian dollars (nominal total returns, %), plus Canadian CPI:
`ca_eq`, `us_eq`, `intl_eq`, `ca_bonds`, `cash`, `cpi`. It is built from
public source data in `historical_data/` by `historical_data/build_returns.py`;
the file header lists every source and splice point. Rebuild it with:

```
python3 historical_data/build_returns.py --check
```

(needs Python with pandas, openpyxl and xlrd; `--check` prints the
cross-checks between overlapping sources).

Monte Carlo reads this file directly when the config sets portfolio weights
(fractions summing to 1; the portfolio is rebalanced to them every year):

```
mc_returns_file=historical_returns_cad.csv
mc_alloc_ca_eq=0.20
mc_alloc_us_eq=0.20
mc_alloc_intl_eq=0.20
mc_alloc_ca_bonds=0.30
mc_alloc_cash=0.10
```

Leave out an asset class (or set it to 0) to hold none of it. Set
`annual_fee_pct` to your fees, since these are index returns before fees,
and run with `--use-historical-cpi`: returns from high-inflation decades are
only meaningful alongside that era's inflation.

#### Expected returns (optional)

History's average return is one period's outcome, and 1957–2025 included
interest rates falling from 15% to near zero, which flattered both bonds and
stocks. To test a plan against a forward-looking average while keeping
history's volatility, set an expected long-run **real** (after-inflation)
return per asset class:

```
mc_real_return_ca_eq=4.1
mc_real_return_us_eq=4.2
mc_real_return_intl_eq=4.4
mc_real_return_ca_bonds=1.1
mc_real_return_cash=0.3
```

For each asset with a target, every year's (1 + return) is multiplied by one
constant, chosen so the asset's compound real return over the whole file
equals the target. Each year keeps its CPI, its crash or boom relative to the
average, and its co-movement with the other assets; only the average moves.
The run prints each asset's historical real return, the target and the
multiplier. Assets without a target use pure history.

The values above are the FP Canada / Institute of Financial Planning 2026
[Projection Assumption Guidelines](https://institutpf.org/assets/Documents/Normes-projection/InstituteFP-PAG2026.pdf)
(nominal 6.3 / 6.4 / 6.6 / 3.2 / 2.4%) less their 2.1% inflation assumption.
Their equity figures include a 0.5% safety margin, which the guidelines
suggest adding back for Monte Carlo (4.6 / 4.7 / 4.9), since Monte Carlo
models the uncertainty itself. Update the values when new guidelines are
published each spring. For comparison, 1957–2025 history is 5.5 / 7.3 / 7.0 /
2.5 / 1.6% real.

The script can also write a pre-blended 3-column file for one allocation
(`--blend ca_eq=0.2,...,cash=0.1 my-blend.csv`);
`blend-20ca-20us-20intl-30bond-10cash.csv` is that example.

Known limitations:
- `intl_eq` before 1999 is a stand-in (GDP-weighted average of 15 developed
  markets from the JST Macrohistory Database). It tracks MSCI EAFE closely
  year to year but ran about 2 points a year higher over 1999–2020, so it is
  likely optimistic before 1999.
- `ca_bonds` is approximated from Government of Canada bond yields, so it
  has no corporate or provincial bonds. It runs about 0.6 points a year below
  a broad bond index, and it looks better than a bond fund in credit crises
  (2008) and worse in the recoveries (2009).
- Bonds and cash benefited from the falling-rate era of 1981–2000; their
  2001–2025 real returns (about 1% and 0%) are closer to today's yields.

#### Spending guardrails (`--with-guardrails`)

Real retirees don't spend a fixed amount through a market crash; they tighten
up. With `--with-guardrails`, each Monte Carlo trial does the same:

- First the simulator runs your **flat plan** (`roi_annual_pct` less
  `annual_fee_pct`, fixed inflation) and records its total balance at each
  December 31, in today's dollars.
- Each January, a trial compares its own balance (in today's dollars, using
  its own historical inflation) with the plan's on the same date.
- More than `guardrail_trigger_pct` (default 15%) behind: spending is cut a
  further `guardrail_step_pct` (default 5%), up to `guardrail_max_cut_pct`
  (default 20%). Between the trigger and the plan: the current cut is held.
  Back on or ahead of the plan's path: full spending is restored.
- A cut scales the whole net spending target (including rent) and the
  per-person gross target, so registered draws drop too.

The report adds how many trials needed a cut, whether money still ran out,
the deepest cut reached, and how many years cuts lasted. The per-trial CSV
gains `guardrail_years_cut`, `guardrail_max_cut_pct` and
`guardrail_first_cut_year`. "Success" with guardrails means meeting the
reduced target while a cut is in force.

Guardrails steer each trial back towards the flat plan, so **they only help
if the flat plan itself lasts the whole horizon.** If the flat run reports
"money ran out", fix the plan first; the Monte Carlo run prints a note when
this happens.

### Older data files

All in the `year,return_pct,cpi_pct` format, sharing the same Canadian CPI
for overlapping years. These were LLM-generated and are kept for comparison;
checked against MSCI data for 1999–2020 they have known problems:
- `perplexity-40-year.csv` (1987–2025): blend
  of MSCI EAFE, TSX and S&P 500 at 20% each plus a flat 2% on the other 40%.
  The S&P and EAFE components are USD returns (no currency effect), and the
  flat 2% understates fixed income; together it runs about 1.5 points a year
  below the new data in real terms over the same years.
- `proxy-30-year.csv` (1996–2025): estimate for a 10/60/30 portfolio.
- `sp-30-year.csv` (1996–2025): does not match S&P 500 returns in either
  USD or CAD (about 7 points a year off on average).
- `tsx.csv` (1947–2025): TSX **price** returns only (no dividends), about 3
  points a year too low.
- `canada_inflation_1986_2025.csv`: Canadian inflation only; not loadable.
- `example_returns.csv`: placeholder values only, not real data.

## Getting started

1. Copy `sample_config.cfg` to a real config file and enter your actual
   values.
2. Adjust the tax brackets and age credit values to your jurisdiction.
3. Run the simulator with `--config` and review the generated CSV.
4. To run Monte Carlo, point `mc_returns_file` at a file in the
   `year,return_pct,cpi_pct` format (the sample config already uses
   `perplexity-40-year.csv`) and add `--monte-carlo` or `--mc-in-turn`.
