# Retirement Spending Simulator (C)

A standalone retirement spending simulator that models monthly cash flows,
CPP/OAS, tax, registered account withdrawals, TFSA use, and one-time events.
This application reads a configuration file and optionally an events file,
then writes a month-by-month projection of retirement balances and income.

To keep it simple, this program does not include a pre-retirement/income phase.  
You will need to use other tools to forecast your savings at retirement.  As 
such the 'retire-date' configuration is no longer very relevant.

This simulation assumes the government increases CPP/OAS/tax brackets/tax credits 
each year by a configurable inflation factor (default 2%). Which may not be true.

Monte-Carlo scores are based on provided historical data.  A future enhancement 
could be to include a t-skewed student-t distribution of returns.  Aside from coding 
this would require significant user knowledge to set the configuarable properties.

## Build & run

From the project directory:

```
cc -std=c11 -Wall -Wextra -O2 -o retirement_sim main.c config.c tax.c sim.c -lm
```

or

```
make
```

Then run:

```
./retirement_sim --config sample_config.cfg
```

The default output is `retirement_projection.csv` and an annual summary is
printed to the console.

Run `./retirement_sim --help` for CLI options, including overrides for:
- `--roi`
- `--inflation`
- `--gov-inflation`
- `--target-net`
- `--target-gross`
- `--years`
- `--retire-date`
- `--tfsa`
- `--registered-a`
- `--registered-b`
- `--monte-carlo`
- `--mc-in-turn`

## What the simulator models

The simulator advances month by month. For each month it:

1. Computes CPP and OAS income for each person, applying the standard
   early/late adjustment factor:
   - CPP starts early: 0.6% reduction per month early.
   - CPP starts late: 0.7% bonus per month late.
   - OAS starts late: 0.6% bonus per month late.

2. Computes investment income from non-registered balances at the current
   annual ROI rate.

3. Determines each person’s target gross income for retirement months using
   `target_gross_monthly` and personal spending inflation.

4. Draws from each person’s registered account (RRSP/RRIF/LIF) to reach their
   individual gross target, subject to the legal RRIF minimum withdrawal.
   - The RRIF minimum is recalculated each January 1 from that account’s
     opening balance and age, then spread evenly across the year.
   - If one person’s registered account is empty, the other person’s account
     can supply cash to cover the gross draw while keeping the original tax
     attribution.

5. Computes tax and age credits separately for each person, using the
   configured progressive tax brackets and the 65+ age credit rules.

6. If household net income after tax is below the inflated household spending
   target (`target_net_monthly` plus rent if applicable), the simulator
   covers the shortfall in this order:
   - non-registered balances first, splitting the draw proportionally across
     the two non-reg accounts if both exist.
   - TFSA next, up to the available TFSA balance.
   - any remaining shortfall is covered by an additional taxable draw from the
     remaining registered capacity, sized iteratively so that the extra
     withdrawal nets the needed cash after tax.

7. Applies scheduled one-time events in the month they occur.

8. Applies annual inflation and return updates on each January 1:
   - `personal_inflation_pct` inflates spending targets and rent.
   - `gov_inflation_pct` inflates CPP/OAS amounts, tax brackets, and credits.
   - `tfsa_contribution_room` increases by the configured annual grant.
   - non-registered cash is swept into TFSA if contribution room is available.

## Withdrawal rules and order

The simulator uses this withdrawal order when a retirement spending gap exists:

1. Registered account gross draw to satisfy each person’s `target_gross_monthly`.
2. Non-registered account draw to cover any net shortfall.
3. TFSA draw to cover remaining shortfall.
4. Additional registered draw if needed to cover what TFSA cannot.

Important details:
- Registered account withdrawals are first used to satisfy the gross income
  target and RRIF minimum requirement.
- A spouse’s registered account may backstop the other spouse’s shortfall
  while preserving the original taxable income split.
- TFSA is used only after non-registered cash is exhausted.
- If the household still cannot meet the spending target after all available
  sources are drawn, the run is marked as money exhausted.

## Inflation definitions

The simulator tracks two separate inflation rates:

- `personal_inflation_pct`:
  - Applies to spending targets like `target_net_monthly` and
    `target_gross_monthly`.
  - Also increases rent if `rent_monthly` is set.
  - Represents the inflation of household living costs.

- `gov_inflation_pct`:
  - Applies to CPP/OAS benefit amounts, tax brackets, and age-credit
    thresholds.
  - Represents government-indexed benefit and tax inflation.

This separation lets the model treat living-cost inflation and government
benefit/tax inflation independently.

## Monte Carlo simulation

Monte Carlo mode resamples a historical annual return series to test many
possible return paths.

- Input file: `mc_returns_file` in the config holds one annual return value
  per usable line.
- `--monte-carlo` enables standard bootstrap mode, where each simulated year
  draws a random annual return from the historical series with replacement.
- `--mc-in-turn` runs one trial per starting year in the historical file,
  then wraps around if the simulation horizon is longer than the file.

Monte Carlo output includes:
- Success rate: percentage of trials where money never ran out.
- Ending balance percentiles across trials (p10, p25, median, p75, p90).
- A detail CSV `monte_carlo_results.csv` with trial-by-trial outcomes.

## Inputs

`sample_config.cfg` defines the simulation assumptions:
- personal data: DOB, CPP/OAS start dates, benefit amounts.
- account balances: TFSA, registered, non-registered.
- spending targets: `target_net_monthly`, `target_gross_monthly`.
- inflation rates and ROI.
- tax brackets, age-credit rules, rent, one-time events.

`sample_events.csv` lists dated one-time cash flows such as home sales,
renovations, or downsizing. Positive amounts are inflows; negative amounts are
expenses.

### Sample events format

The events file is a simple CSV with one event per row. Each event has:
- `date`: year-month-day when the event occurs
- `amount`: positive for cash inflow, negative for expense
- `note`: a short description of the event

Example rows:

```
2028-06-30,120000,home sale proceeds
2030-09-01,-45000,kitchen renovation
2035-03-15,30000,downsizing relocation cash
```

Events are applied in the month they occur and affect the shared cash
balances used by the simulation.

## Output

The simulator writes `retirement_projection.csv` containing monthly cash flow
and balance details, and prints an annual summary to the console. In Monte
Carlo mode it also writes `monte_carlo_results.csv` with per-trial outcomes.

## File overview

- `main.c`: CLI parsing and program entry.
- `config.c` / `retirement_sim.h`: config parsing, defaults, events loading,
  and simulation data structures.
- `tax.c`: progressive tax calculation and age-credit logic.
- `sim.c`: monthly simulation engine and output generation.
- `monte_carlo.c`: Monte Carlo trial runner and bootstrap logic.
- `dateutil.h`: date utilities used by the monthly loop.
- `sample_config.cfg`: example configuration file to edit for your case.
- `sample_events.csv`: example one-time events schedule.
- `example_returns.csv`: example of historical investment returns for Monte-Carlo.
- `proxy-30-year.csv`: Perplexity generated estimate of 30 year returns for a 10/60/30 portfolio based on weighted averages.
- `sp-30-year.csv`: Perplexity generated report on 30 years of S&P 500
- `tsx.csv`:  Perplexity generated report of 75 years of TSX.

## Getting started

1. Copy `sample_config.cfg` to a real config file and enter your actual
   values.
2. Adjust the tax brackets and age credit values to your jurisdiction.
3. Run the simulator with `--config` and review the generated CSV.
4. If you have a historical return series, enable Monte Carlo mode to
   quantify portfolio survival chances.
