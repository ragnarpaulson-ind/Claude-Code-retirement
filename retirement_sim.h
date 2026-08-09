#ifndef RETIREMENT_SIM_H
#define RETIREMENT_SIM_H

#include "dateutil.h"

#define MAX_TAX_BRACKETS 16
#define MAX_EVENTS 256
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
} SimResult;

/* Full simulation entry point. If roi_override is non-NULL, it must point to
   an array of c->sim_years annual ROI percentages (one per simulated
   calendar year, indexed by year-offset from sim_start_date) used instead of
   the flat c->roi_annual_pct -- this is how Monte Carlo drives a different
   return path through each trial. If quiet is nonzero, all console output is
   suppressed and csv_path may be NULL to skip file output entirely (used for
   fast repeated Monte Carlo trials -- writing a CSV per trial would be far
   too slow across thousands of runs). result_out, if non-NULL, is filled
   with the ending balances and depletion outcome. Returns 0 only if csv_path
   was given but couldn't be opened; 1 otherwise. */
int run_simulation_ex(const Config *c, const char *csv_path, const double *roi_override,
                       int quiet, SimResult *result_out);

/* Runs c->mc_trials trials, each resampling a random annual return path from
   c->mc_returns_file (one year picked independently with replacement per
   simulated year -- see monte_carlo.c for the file format). Prints a summary
   (success rate, ending-balance percentiles) and writes a per-trial CSV
   (monte_carlo_results.csv, in the same directory as csv_path if given, else
   the current directory) for further analysis. Returns 0 on failure to load
   the returns file, 1 on success. */
int run_monte_carlo(const Config *c, const char *csv_path);
#endif
