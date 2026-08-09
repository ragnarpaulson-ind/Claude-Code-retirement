#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include "retirement_sim.h"

/* Historical returns file format: one annual % return per non-blank,
   non-comment line, e.g.:
       # a rough placeholder blended return series -- replace with real data
       7.2
       -3.1
       11.8
       ...
   Lines starting with '#' and blank lines are ignored. Whatever's on a line
   before the first comma (if any) is used, so "1994,7.2" style lines also
   work if you're pasting straight from a spreadsheet export. */
static int load_return_series(const char *path, double **out, int *n) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Could not open historical returns file '%s'\n", path);
        return 0;
    }

    int capacity = 128, count = 0;
    double *arr = malloc(capacity * sizeof(double));
    char line[256];

    while (fgets(line, sizeof(line), f)) {
        char *l = line;
        while (isspace((unsigned char)*l)) l++;
        if (*l == 0 || *l == '#') continue;

        /* accept either "return" or "year,return" -- use the last comma-
           separated field so a leading year column doesn't break atof */
        char *last_field = l;
        for (char *p = l; *p; p++) {
            if (*p == ',') last_field = p + 1;
        }
        double val = atof(last_field);

        if (count >= capacity) {
            capacity *= 2;
            arr = realloc(arr, capacity * sizeof(double));
        }
        arr[count++] = val;
    }
    fclose(f);

    if (count == 0) {
        fprintf(stderr, "Historical returns file '%s' had no usable data rows.\n", path);
        free(arr);
        return 0;
    }

    *out = arr;
    *n = count;
    return 1;
}

static int compare_doubles(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

/* Returns the value at the given percentile (0-100) of a SORTED array. */
static double percentile(const double *sorted, int n, double pct) {
    if (n == 1) return sorted[0];
    double idx = (pct / 100.0) * (n - 1);
    int lo = (int)idx;
    int hi = lo + 1 < n ? lo + 1 : lo;
    double frac = idx - lo;
    return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
}

int run_monte_carlo(const Config *c, const char *csv_path) {
    if (c->mc_returns_file[0] == '\0') {
        fprintf(stderr, "mc_returns_file is not set -- Monte Carlo needs a historical annual-return series to bootstrap from.\n");
        return 0;
    }

    double *returns = NULL;
    int n_returns = 0;
    if (!load_return_series(c->mc_returns_file, &returns, &n_returns)) return 0;

    int trials = c->mc_trials > 0 ? c->mc_trials : 10000;
    if (c->mc_in_turn) {
        trials = n_returns;
        if (trials < 1) {
            fprintf(stderr, "Monte Carlo in-turn mode requested, but historical returns file has no usable data.\n");
            free(returns);
            return 0;
        }
    }
    /* calculate the cumulative expected percentage return in the trials  and the fixed yearly 
       equivalent based on the starting historial data  */
    double cumulative_return = 1.0;
    for (int i = 0; i < n_returns; i++) {
        cumulative_return *= (1.0 + returns[i] / 100.0);
    }
  
    double fixed_yearly_return = pow(cumulative_return, 1.0 / n_returns) - 1.0;
    cumulative_return -= 1.0;  /* convert to percentage */
    printf("Historical returns file '%s' has %d usable years, cumulative return %.2f%%, fixed yearly equivalent %.2f%%\n",
           c->mc_returns_file, n_returns, (cumulative_return) * 100.0, fixed_yearly_return * 100.0);


    srand(c->mc_seed != 0 ? c->mc_seed : (unsigned int)time(NULL));

    double *roi_by_year = malloc(c->sim_years * sizeof(double));
    double *ending_totals = malloc(trials * sizeof(double));
    int *ran_out_years = malloc(trials * sizeof(int));  /* -1 if never ran out */

    int successes = 0;

    /* Per-trial CSV, for anyone who wants to look at the full distribution
       (histogram, scatter, etc.) beyond the summary percentiles printed
       below. Placed alongside the main projection CSV if a path was given,
       else in the current directory. */
    char mc_csv_path[512] = "monte_carlo_results.csv";
    if (csv_path) {
        const char *slash = strrchr(csv_path, '/');
        if (slash) {
            size_t dir_len = (size_t)(slash - csv_path) + 1;
            if (dir_len < sizeof(mc_csv_path) - 32) {
                memcpy(mc_csv_path, csv_path, dir_len);
                strcpy(mc_csv_path + dir_len, "monte_carlo_results.csv");
            }
        }
    }
    FILE *mc_csv = fopen(mc_csv_path, "w");
    if (mc_csv) {
        fprintf(mc_csv, "trial,success,ending_total,ending_tfsa,ending_registered_a,ending_registered_b,"
                        "ending_nonreg_a,ending_nonreg_b,money_ran_out_year\n");
    }

    for (int t = 0; t < trials; t++) {
        if (c->mc_in_turn) {
            /* In-turn mode: each trial uses a different starting year from the
               historical returns file, then wraps around to the beginning of
               the file if the simulation is longer than the number of years in
               the file. */
            for (int y = 0; y < c->sim_years; y++) {
                int idx = (t + y) % n_returns;
                roi_by_year[y] = returns[idx];
            }
        } else {
            /* Standard bootstrap resampling: each trial draws a random year
               independently with replacement for each simulated year. */
            for (int y = 0; y < c->sim_years; y++) {
                roi_by_year[y] = returns[rand() % n_returns];
            }
        }
        
        SimResult result;
        run_simulation_ex(c, NULL, roi_by_year, 1, &result);

        ending_totals[t] = result.ending_total;
        ran_out_years[t] = result.money_ran_out ? result.money_ran_out_date.year : -1;
        if (!result.money_ran_out) successes++;

        if (mc_csv) {
            fprintf(mc_csv, "%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d\n",
                    t + 1, !result.money_ran_out, result.ending_total, result.ending_tfsa,
                    result.ending_registered_a, result.ending_registered_b,
                    result.ending_nonreg_a, result.ending_nonreg_b, ran_out_years[t]);
        }
    }
    if (mc_csv) fclose(mc_csv);

    double *sorted_totals = malloc(trials * sizeof(double));
    memcpy(sorted_totals, ending_totals, trials * sizeof(double));
    qsort(sorted_totals, trials, sizeof(double), compare_doubles);

    /* Among trials that ran out, the distribution of *when* -- helpful for
       seeing whether failures cluster late (minor shortfall) or early
       (structural problem), separate from the pass/fail rate itself. */
    int n_failed = trials - successes;
    int *fail_years = NULL;
    if (n_failed > 0) {
        fail_years = malloc(n_failed * sizeof(int));
        int fi = 0;
        for (int t = 0; t < trials; t++) if (ran_out_years[t] >= 0) fail_years[fi++] = ran_out_years[t];
        /* simple insertion sort -- n_failed is at most `trials`, fine at MC scale */
        for (int i = 1; i < n_failed; i++) {
            int key = fail_years[i], j = i - 1;
            while (j >= 0 && fail_years[j] > key) { fail_years[j+1] = fail_years[j]; j--; }
            fail_years[j+1] = key;
        }
    }

    printf("\n=== Monte Carlo (%d trials, bootstrapped from %d historical years in '%s') ===\n",
           trials, n_returns, c->mc_returns_file);
    printf("Success rate (money never ran out): %.1f%% (%d/%d trials)\n",
           100.0 * successes / trials, successes, trials);
    printf("\nEnding total balance (TFSA + registered + non-registered), across all trials:\n");
    printf("  p10: $%.0f\n", percentile(sorted_totals, trials, 10));
    printf("  p25: $%.0f\n", percentile(sorted_totals, trials, 25));
    printf("  p50 (median): $%.0f\n", percentile(sorted_totals, trials, 50));
    printf("  p75: $%.0f\n", percentile(sorted_totals, trials, 75));
    printf("  p90: $%.0f\n", percentile(sorted_totals, trials, 90));
    if (n_failed > 0) {
        printf("\nOf the %d trial(s) where money ran out, the year it first happened:\n", n_failed);
        printf("  earliest: %d\n", fail_years[0]);
        printf("  median:   %d\n", fail_years[n_failed / 2]);
        printf("  latest:   %d\n", fail_years[n_failed - 1]);
    }
    printf("\nPer-trial detail written to: %s\n", mc_csv_path);

    free(returns);
    free(roi_by_year);
    free(ending_totals);
    free(ran_out_years);
    free(sorted_totals);
    free(fail_years);

    return 1;
}
