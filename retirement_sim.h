#ifndef RETIREMENT_SIM_H
#define RETIREMENT_SIM_H

#include "dateutil.h"

#define MAX_TAX_BRACKETS 16
#define MAX_EVENTS 256

/* Asset classes in historical_returns_cad.csv, in config/key order. */
#define N_ASSET_CLASSES 5
#define MAX_PATH_YEARS 128   /* calendar years a simulation can span */
extern const char *const ASSET_CLASS_NAMES[N_ASSET_CLASSES];
#define WHO_NONE 0
#define WHO_A 1   /* "person A" -- e.g. Ragnar */
#define WHO_B 2   /* "person B" -- e.g. Jen */

typedef struct {
    double lower_limit;    /* annual taxable income where this bracket starts */
    double marginal_rate;  /* combined federal+provincial rate in this bracket */
} TaxBracket;

typedef struct {
    Date date;
    double amount;       /* + = windfall/inflow (home sale, downsize), - = expense (roof, car) */
    char note[128];
} OneTimeEvent;

typedef struct _historical_returns {
	int	year;
	float   roi;
	float	cpi;
} historical_returns_t;

typedef struct {
    char name[32];
    Date dob;
    Date cpp_start;
    Date oas_start;
    double cpp_at_65;    /* monthly CPP amount if started exactly at 65, today's dollars */
    double oas_at_65;    /* monthly OAS amount if started exactly at 65, today's dollars */
} Person;

typedef struct {
    Person a;             /* e.g. Ragnar */
    Person b;             /* e.g. Jen */

    Date retirement_date;
    Date sim_start_date;
    Date rent_start_date;
    int  sim_years;

    double tfsa_balance;              /* combined starting TFSA balance */
    double tfsa_contribution_room;       /* combined starting TFSA contribution room */
    double tfsa_annual_contribution_room_grant; /* combined yearly TFSA contribution room increase (e.g. $6,500) */
    double person_a_registered_balance;  /* starting RRSP/RRIF/LIF balance, this person's own account */
    double person_b_registered_balance;  /* starting RRSP/RRIF/LIF balance, this person's own account */
    double person_a_nonreg_balance;     /* starting non-registered balance, this person's own account */
    double person_b_nonreg_balance;     /* starting non-registered balance, this person's own account */

    double roi_annual_pct;           /* investment return, applied to all pools */
    double annual_fee_pct;           /* investment fees (e.g. ETF MERs), deducted from
                                        every year's return, flat and Monte Carlo */
    double personal_inflation_pct;   /* inflates spending targets */
    double gov_inflation_pct;        /* inflates CPP/OAS/tax brackets/credits */

    double target_net_monthly;       /* today's-dollars net (after-tax) spending target */
    double target_gross_monthly;     /* today's-dollars gross (pre-tax) household income target */

    int    go_slow_year;             /* calendar year spending stops getting inflation raises */
    int    go_slow_resume_after_years;

    TaxBracket brackets[MAX_TAX_BRACKETS];
    int n_brackets;

    double fed_age_credit_amount;
    double fed_age_credit_rate;
    double fed_age_credit_clawback_start;
    double prov_age_credit_amount;
    double prov_age_credit_rate;
    double prov_age_credit_clawback_start;
    double age_credit_clawback_rate;

    double rent_monthly;  /* if >0, monthly rent expense in today's dollarsstarting at rent_start_date */

    OneTimeEvent events[MAX_EVENTS];
    int n_events;
    /* Monte Carlo: bootstrap resampling of a historical annual-return series,
       one independently-drawn year at a time (see mc_returns_file format
       note in monte_carlo.c). Only ROI is randomized -- inflation stays at
       the deterministic personal_inflation_pct/gov_inflation_pct rates
       above. */
    int    mc_trials;                 /* number of simulated lifetimes to run, e.g. 10000 */
    char   mc_returns_file[256];      /* path to a file of historical annual % returns, one per line */
    unsigned int mc_seed;             /* 0 = seed from current time (non-reproducible) */
    unsigned int mc_run;              /* 0, run single simulation, 1 run monte-carlo. set by CLI */
    unsigned int mc_in_turn;          /* 0, run simulations based on historical date set size, starting from each year in set once. set by CLI */
    unsigned int mc_use_historical_cpi;  /* 0, use fixed gov_inflation_pct. 1, use historical CPI from mc_returns_file. set by CLI */
    /* Portfolio weights (fractions summing to 1, rebalanced annually) used to
       blend an asset-class returns file such as historical_returns_cad.csv
       into one return per year. All zero = the file already holds a single
       blended return (year,return_pct,cpi_pct). Config keys mc_alloc_<name>. */
    double mc_alloc[N_ASSET_CLASSES];
    /* Optional expected long-run REAL (after-inflation) return per asset
       class, % per year (keys mc_real_return_<name>). When set, that asset's
       historical returns are re-centred so their compound real average over
       the file equals this value; year-to-year deviations, crashes,
       co-movement and each year's CPI are kept. Unset = pure history. */
    double mc_real_return[N_ASSET_CLASSES];
    int    mc_real_return_set[N_ASSET_CLASSES];
    /* Spending guardrails (Monte Carlo, --with-guardrails). Each January the
       trial's total balance in today's dollars is compared with the flat
       plan's (roi_annual_pct, fixed inflation) at the same date. More than
       guardrail_trigger_pct behind: cut spending another guardrail_step_pct,
       up to guardrail_max_cut_pct. Back on (or ahead of) the plan: restore
       fully. Cuts apply to the whole net target (incl. rent) and the
       per-person gross target. */
    double guardrail_trigger_pct;
    double guardrail_step_pct;
    double guardrail_max_cut_pct;
    unsigned int mc_with_guardrails;  /* set by --with-guardrails */
} Config;

void config_defaults(Config *c);
int  config_load(Config *c, const char *path);
int  events_load(Config *c, const char *path);
void config_apply_overrides(Config *c, int argc, char **argv);

double tax_owed_annual(const Config *c, double annual_taxable_income, double gov_inflate_factor);
double age_credit_annual(const Config *c, double annual_taxable_income, double gov_inflate_factor);
double rrif_min_factor(int age_on_jan1);

int run_simulation(const Config *c, const char *csv_path);




/* Outcome of a single simulation run -- used directly by Monte Carlo, and
   available to any other caller that wants the ending state without
   re-parsing the CSV. */
typedef struct {
    int    money_ran_out;         /* 1 if the household was ever short of target after all sources (registered + non-reg + TFSA) were drawn as far as possible */
    Date   money_ran_out_date;    /* first month this happened; only meaningful if money_ran_out is 1 */
    double ending_tfsa;
    double ending_registered_a;
    double ending_registered_b;
    double ending_nonreg_a;
    double ending_nonreg_b;
    double ending_total;          /* sum of all five balances above, for a quick single-number outcome */
    /* Total balance at each Dec 31 in today's dollars (deflated by cumulative
       spending inflation), index = year - sim_start year. Used as the flat
       plan's path for guardrails. */
    int    n_year_ends;
    double year_end_real_total[MAX_PATH_YEARS];
    /* Guardrail outcome (zero when guardrails are off) */
    int    guardrail_years_cut;       /* calendar years spent with any cut */
    double guardrail_max_cut_pct;     /* deepest cut reached */
    int    guardrail_first_cut_year;  /* 0 if never cut */
} SimResult;

/* Full simulation entry point. If roi_override is non-NULL, it must point to
   an array with one entry per simulated CALENDAR year (indexed by
   year-offset from sim_start_date; that is sim_years + 1 entries when the
   run starts partway through a year) used instead of
   the flat c->roi_annual_pct -- this is how Monte Carlo drives a different
   return path through each trial. If quiet is nonzero, all console output is
   suppressed and csv_path may be NULL to skip file output entirely (used for
   fast repeated Monte Carlo trials -- writing a CSV per trial would be far
   too slow across thousands of runs). result_out, if non-NULL, is filled
   with the ending balances and depletion outcome. Returns 0 only if csv_path
   was given but couldn't be opened; 1 otherwise. */
/* plan: the flat plan's result (for its year_end_real_total path); non-NULL
   turns guardrails on for this run. */
int run_simulation_ex(const Config *c, const char *csv_path, const historical_returns_t *roi_override,
                       int quiet, SimResult *result_out, const SimResult *plan);

/* Runs c->mc_trials trials, each resampling a random annual return path from
   c->mc_returns_file (one year picked independently with replacement per
   simulated year -- see monte_carlo.c for the file format). Prints a summary
   (success rate, ending-balance percentiles) and writes a per-trial CSV
   (monte_carlo_results.csv, in the same directory as csv_path if given, else
   the current directory) for further analysis. Returns 0 on failure to load
   the returns file, 1 on success. */
int run_monte_carlo(const Config *c, const char *csv_path);
#endif
