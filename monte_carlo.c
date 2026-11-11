#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include "retirement_sim.h"

/* Historical returns file formats. Lines starting with '#' and blank lines
   are ignored, except a "# year,..." comment, which names the columns.
   Years must be consecutive. Values may carry a trailing '%'.

   1. Single blended return (no mc_alloc_* keys set):
          1987,8.02,4.36            year,return_pct,cpi_pct

   2. Asset-class returns, e.g. historical_returns_cad.csv:
          # year,ca_eq,us_eq,intl_eq,ca_bonds,cash,cpi
          1957,-20.58,-1.27,...
      Requires mc_alloc_<name> weights in the config (fractions summing to
      1). Each year's return is the weighted sum of the named columns, i.e.
      the portfolio is rebalanced to those weights every year; cpi comes
      from the "cpi" column.
*/

#define MAX_RETURN_COLS 16

/* Parses comma-separated numbers into vals; returns how many were read. */
static int parse_numbers(const char *l, double *vals, int max) {
    int n = 0;
    while (*l && n < max) {
        char *end;
        double v = strtod(l, &end);
        if (end == l) break;                       /* not a number */
        vals[n++] = v;
        l = end;
        while (*l && *l != ',') l++;               /* skip '%', spaces */
        if (*l == ',') l++;
    }
    return n;
}

/* If l (after '#') is a "year,..." header, fills names and returns the
   number of columns; otherwise returns 0. */
static int parse_header(const char *l, char names[][32], int max) {
    while (*l == '#' || isspace((unsigned char)*l)) l++;
    if (strncmp(l, "year,", 5) != 0) return 0;
    int n = 0;
    while (*l && n < max) {
        while (isspace((unsigned char)*l)) l++;
        int k = 0;
        while (*l && *l != ',' && !isspace((unsigned char)*l) && k < 31) names[n][k++] = *l++;
        names[n][k] = 0;
        n++;
        while (*l && *l != ',') l++;
        if (*l == ',') l++;
    }
    return n;
}

static int find_column(char names[][32], int n_names, const char *name) {
    for (int i = 0; i < n_names; i++)
        if (strcmp(names[i], name) == 0) return i;
    return -1;
}

static int load_return_series(const Config *c, const char *path, historical_returns_t **out, int *n) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Could not open historical returns file '%s'\n", path);
        return 0;
    }

    int use_alloc = 0;
    for (int a = 0; a < N_ASSET_CLASSES; a++) if (c->mc_alloc[a] != 0.0) use_alloc = 1;

    char names[MAX_RETURN_COLS][32];
    int n_names = 0;
    int alloc_col[N_ASSET_CLASSES], cpi_col = -1;   /* data-value index (year excluded) */

    int capacity = 128, count = 0;
    historical_returns_t *returns = malloc(capacity * sizeof(*returns));
    /* per-year asset-class returns, kept until all years are read so each
       asset can be re-centred on its mc_real_return_* target before blending */
    double (*asset)[N_ASSET_CLASSES] = malloc(capacity * sizeof(*asset));
    if (!returns || !asset) {
        fprintf(stderr, "Out of memory reading historical returns file '%s'\n", path);
        free(returns);
        free(asset);
        fclose(f);
        return 0;
    }
    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        char *l = line;
        while (isspace((unsigned char)*l)) l++;
        if (*l == '#') {
            int k = parse_header(l, names, MAX_RETURN_COLS);
            if (k > 0) n_names = k;
            continue;
        }
        if (*l == 0) continue;

        double vals[MAX_RETURN_COLS];
        int nv = parse_numbers(l, vals, MAX_RETURN_COLS);
        if (nv < 3) {
            fprintf(stderr, "Invalid historical returns file '%s': need year,return,cpi on line: %s", path, line);
            goto fail;
        }

        if (count == 0) {
            /* Resolve columns once, from the header seen before the first row. */
            if (use_alloc) {
                if (n_names == 0) {
                    fprintf(stderr, "mc_alloc_* weights are set, but '%s' has no \"# year,...\" header naming its columns.\n", path);
                    goto fail;
                }
                for (int a = 0; a < N_ASSET_CLASSES; a++) {
                    alloc_col[a] = find_column(names, n_names, ASSET_CLASS_NAMES[a]) - 1;
                    if (c->mc_alloc[a] != 0.0 && alloc_col[a] < 0) {
                        fprintf(stderr, "mc_alloc_%s is set, but '%s' has no '%s' column.\n",
                                ASSET_CLASS_NAMES[a], path, ASSET_CLASS_NAMES[a]);
                        goto fail;
                    }
                }
                cpi_col = find_column(names, n_names, "cpi") - 1;
                if (cpi_col < 0) {
                    fprintf(stderr, "'%s' has no 'cpi' column.\n", path);
                    goto fail;
                }
            } else if (nv > 3) {
                fprintf(stderr, "'%s' has %d return columns (asset classes?). Set mc_alloc_* weights in the config "
                                "to blend them, or use a year,return_pct,cpi_pct file.\n", path, nv - 1);
                goto fail;
            }
        }

        if (count >= capacity) {
            capacity *= 2;
            historical_returns_t *grown = realloc(returns, capacity * sizeof(*returns));
            if (grown) returns = grown;
            double (*grown_a)[N_ASSET_CLASSES] = realloc(asset, capacity * sizeof(*asset));
            if (grown_a) asset = grown_a;
            if (!grown || !grown_a) {
                fprintf(stderr, "Out of memory reading historical returns file '%s'\n", path);
                goto fail;
            }
        }

        returns[count].year = (int)vals[0];
        if (count > 0 && returns[count-1].year + 1 != returns[count].year) {
            fprintf(stderr, "Invalid historical returns file, year %d followed by %d\n",
                    returns[count-1].year, returns[count].year);
            goto fail;
        }
        if (use_alloc) {
            for (int a = 0; a < N_ASSET_CLASSES; a++) {
                asset[count][a] = 0.0;
                if (c->mc_alloc[a] == 0.0) continue;
                if (alloc_col[a] + 1 >= nv) {
                    fprintf(stderr, "'%s': year %d is missing the '%s' value.\n", path, returns[count].year, ASSET_CLASS_NAMES[a]);
                    goto fail;
                }
                asset[count][a] = vals[alloc_col[a] + 1];
            }
            if (cpi_col + 1 >= nv) {
                fprintf(stderr, "'%s': year %d is missing the cpi value.\n", path, returns[count].year);
                goto fail;
            }
            returns[count].roi = 0.0;     /* blended below, once all years are read */
            returns[count].cpi = vals[cpi_col + 1];
        } else {
            returns[count].roi = vals[1];
            returns[count].cpi = vals[2];
        }
        count++;
    }
    fclose(f);

    if (count == 0) {
        fprintf(stderr, "Historical returns file '%s' had no usable data rows.\n", path);
        free(returns);
        free(asset);
        return 0;
    }

    if (use_alloc) {
        /* Re-centre: for each asset with a real-return target, find the
           compound real return over the whole file, g = (prod (1+r)/(1+cpi))^(1/n),
           then multiply every year's (1+r) by k = (1+target)/g. The compound
           real return becomes exactly the target; each year keeps its CPI and
           its deviation from the average (in relative terms). */
        for (int a = 0; a < N_ASSET_CLASSES; a++) {
            if (!c->mc_real_return_set[a] || c->mc_alloc[a] == 0.0) continue;
            double log_sum = 0.0;
            for (int i = 0; i < count; i++)
                log_sum += log((1.0 + asset[i][a] / 100.0) / (1.0 + returns[i].cpi / 100.0));
            double g = exp(log_sum / count);
            double k = (1.0 + c->mc_real_return[a] / 100.0) / g;
            for (int i = 0; i < count; i++)
                asset[i][a] = ((1.0 + asset[i][a] / 100.0) * k - 1.0) * 100.0;
            printf("  %-8s real return: historical %.2f%%/yr -> target %.2f%%/yr (each year's 1+return x %.4f)\n",
                   ASSET_CLASS_NAMES[a], (g - 1.0) * 100.0, c->mc_real_return[a], k);
        }
        for (int i = 0; i < count; i++) {
            double roi = 0.0;
            for (int a = 0; a < N_ASSET_CLASSES; a++) roi += c->mc_alloc[a] * asset[i][a];
            returns[i].roi = roi;
        }
    }
    free(asset);

    *out = returns;
    *n = count;
    return 1;

fail:
    free(returns);
    free(asset);
    fclose(f);
    return 0;
}

/* Checks mc_alloc_* weights: none negative, and (if any are set) summing to 1. */
static int check_allocation(const Config *c) {
    double sum = 0.0;
    int any = 0;
    for (int a = 0; a < N_ASSET_CLASSES; a++) {
        if (c->mc_alloc[a] < 0.0) {
            fprintf(stderr, "mc_alloc_%s is negative (%g).\n", ASSET_CLASS_NAMES[a], c->mc_alloc[a]);
            return 0;
        }
        if (c->mc_alloc[a] != 0.0) any = 1;
        sum += c->mc_alloc[a];
    }
    if (any && fabs(sum - 1.0) > 0.001) {
        fprintf(stderr, "mc_alloc_* weights sum to %.4f; they must sum to 1 (e.g. 0.20 for 20%%).\n", sum);
        return 0;
    }
    for (int a = 0; a < N_ASSET_CLASSES; a++) {
        if (!c->mc_real_return_set[a]) continue;
        if (!any) {
            fprintf(stderr, "mc_real_return_%s is set, but no mc_alloc_* weights are: real-return targets need "
                            "an asset-class returns file and allocation weights.\n", ASSET_CLASS_NAMES[a]);
            return 0;
        }
        if (c->mc_alloc[a] == 0.0)
            fprintf(stderr, "Warning: mc_real_return_%s is set but mc_alloc_%s is 0 (ignored).\n",
                    ASSET_CLASS_NAMES[a], ASSET_CLASS_NAMES[a]);
        if (c->mc_real_return[a] <= -100.0) {
            fprintf(stderr, "mc_real_return_%s must be above -100.\n", ASSET_CLASS_NAMES[a]);
            return 0;
        }
    }
    int any_target = 0;
    for (int a = 0; a < N_ASSET_CLASSES; a++) if (c->mc_real_return_set[a] && c->mc_alloc[a] != 0.0) any_target = 1;
    if (any_target && !c->mc_use_historical_cpi)
        fprintf(stderr, "Warning: mc_real_return_* targets are after inflation using the file's historical CPI; "
                        "run with --use-historical-cpi or spending will not inflate to match.\n");
    if (any) {
        printf("Portfolio allocation (rebalanced annually):");
        for (int a = 0; a < N_ASSET_CLASSES; a++)
            if (c->mc_alloc[a] != 0.0) printf(" %s %.0f%%", ASSET_CLASS_NAMES[a], c->mc_alloc[a] * 100.0);
        printf("\n");
    }
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

    historical_returns_t *returns = NULL;
    int n_returns = 0;
    if (!check_allocation(c)) return 0;
    if (!load_return_series(c, c->mc_returns_file, &returns, &n_returns)) return 0;

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
        cumulative_return *= (1.0 + returns[i].roi / 100.0);
    }
  
    double fixed_yearly_return = pow(cumulative_return, 1.0 / n_returns) - 1.0;
    cumulative_return -= 1.0;  /* convert to percentage */
    printf("Historical returns file '%s' has %d usable years, cumulative return %.2f%%, fixed yearly equivalent %.2f%%\n",
           c->mc_returns_file, n_returns, (cumulative_return) * 100.0, fixed_yearly_return * 100.0);
    if (c->annual_fee_pct != 0.0) {
        double net = 1.0;
        for (int i = 0; i < n_returns; i++) net *= 1.0 + (returns[i].roi - c->annual_fee_pct) / 100.0;
        printf("  after annual_fee_pct %.2f%%: fixed yearly equivalent %.2f%% (fees deducted from every year's return)\n",
               c->annual_fee_pct, (pow(net, 1.0 / n_returns) - 1.0) * 100.0);
    }
    /* calculate the cumulative inflation (cpi) in the historical data and fixed yearly equivalent */
    double cumulative_inflate = 1.0;
    for (int i = 0; i < n_returns; i++) {
        cumulative_inflate *= (1.0 + returns[i].cpi / 100.0);
    }
  
    double fixed_yearly_cpi = pow(cumulative_inflate, 1.0 / n_returns) - 1.0;
    cumulative_inflate -= 1.0;  /* convert to percentage */
    printf("Historical CPI file '%s' has %d usable years, cumulative infation %.2f%%, fixed yearly equivalent %.2f%%\n",
           c->mc_returns_file, n_returns, (cumulative_inflate) * 100.0, fixed_yearly_cpi * 100.0);


    srand(c->mc_seed != 0 ? c->mc_seed : (unsigned int)time(NULL));

    /* sim.c indexes the return path by calendar-year offset from
       sim_start_date. A run that starts partway through a year spans one
       more calendar year than sim_years (e.g. Aug 2026 + 40 years of months
       runs to Jul 2066 = 41 calendar years), so size the path by calendar
       years spanned, not by sim_years. */
    int n_path_years = (c->sim_start_date.month - 1 + c->sim_years * 12 - 1) / 12 + 1;
    historical_returns_t *roi_by_year = malloc(n_path_years * sizeof(historical_returns_t));
    double *ending_totals = malloc(trials * sizeof(double));
    int *ran_out_years = malloc(trials * sizeof(int));  /* -1 if never ran out */

    /* Guardrails: the flat plan (roi_annual_pct net of fees, fixed
       inflation) supplies the path each trial is compared against. */
    SimResult plan;
    const SimResult *plan_ptr = NULL;
    int gr_any_cut = 0, gr_succ_with_cut = 0;
    int *gr_years = NULL;
    int gr_depth_count[64] = {0};      /* trials whose deepest cut is step*(i) */
    int gr_n_steps = 0;
    if (c->mc_with_guardrails) {
        if (c->guardrail_step_pct <= 0.0 || c->guardrail_max_cut_pct < c->guardrail_step_pct ||
            c->guardrail_max_cut_pct >= 100.0 || c->guardrail_trigger_pct <= 0.0 || c->guardrail_trigger_pct >= 100.0) {
            fprintf(stderr, "Invalid guardrails: need 0 < guardrail_step_pct <= guardrail_max_cut_pct < 100 "
                            "and 0 < guardrail_trigger_pct < 100.\n");
            free(returns); free(roi_by_year); free(ending_totals); free(ran_out_years);
            return 0;
        }
        gr_n_steps = (int)(c->guardrail_max_cut_pct / c->guardrail_step_pct + 0.5);
        if (gr_n_steps > 63) gr_n_steps = 63;
        run_simulation_ex(c, NULL, NULL, 1, &plan, NULL);
        plan_ptr = &plan;
        gr_years = calloc(trials, sizeof(int));
        printf("Guardrails ON: each January, if the balance (today's dollars) is more than %.0f%% behind the flat plan\n"
               "  (roi_annual_pct %.2f%% less annual_fee_pct %.2f%%, personal inflation %.2f%%), cut spending a further %.0f%%,\n"
               "  to at most %.0f%%; restore full spending once back on the plan's path.\n",
               c->guardrail_trigger_pct, c->roi_annual_pct, c->annual_fee_pct, c->personal_inflation_pct,
               c->guardrail_step_pct, c->guardrail_max_cut_pct);
        if (plan.money_ran_out)
            printf("  Note: the flat plan itself runs out of money in %04d-%02d; after that there is no path to compare\n"
                   "  against, so trials hold whatever cut they have.\n",
                   plan.money_ran_out_date.year, plan.money_ran_out_date.month);
    }

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
                        "ending_nonreg_a,ending_nonreg_b,money_ran_out_year%s\n",
                        plan_ptr ? ",guardrail_years_cut,guardrail_max_cut_pct,guardrail_first_cut_year" : "");
    }

    for (int t = 0; t < trials; t++) {
        if (c->mc_in_turn) {
            /* In-turn mode: each trial uses a different starting year from the
               historical returns file, then wraps around to the beginning of
               the file if the simulation is longer than the number of years in
               the file. */
            for (int y = 0; y < n_path_years; y++) {
                int idx = (t + y) % n_returns;
                roi_by_year[y] = returns[idx];
            }
        } else {
            /* Standard bootstrap resampling: each trial draws a random year
               independently with replacement for each simulated year. */
            for (int y = 0; y < n_path_years; y++) {
                roi_by_year[y] = returns[rand() % n_returns];
            }
        }
        
        SimResult result;
        run_simulation_ex(c, NULL, roi_by_year, 1, &result, plan_ptr);
        if (plan_ptr) {
            gr_years[t] = result.guardrail_years_cut;
            if (result.guardrail_years_cut > 0) {
                gr_any_cut++;
                if (!result.money_ran_out) gr_succ_with_cut++;
            }
            int d = (int)(result.guardrail_max_cut_pct / c->guardrail_step_pct + 0.5);
            if (d > gr_n_steps) d = gr_n_steps;
            gr_depth_count[d]++;
        }

        ending_totals[t] = result.ending_total;
        ran_out_years[t] = result.money_ran_out ? result.money_ran_out_date.year : -1;
        if (!result.money_ran_out) successes++;

        if (mc_csv) {
            fprintf(mc_csv, "%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d",
                    t + 1, !result.money_ran_out, result.ending_total, result.ending_tfsa,
                    result.ending_registered_a, result.ending_registered_b,
                    result.ending_nonreg_a, result.ending_nonreg_b, ran_out_years[t]);
            if (plan_ptr)
                fprintf(mc_csv, ",%d,%.1f,%d", result.guardrail_years_cut,
                        result.guardrail_max_cut_pct, result.guardrail_first_cut_year);
            fprintf(mc_csv, "\n");
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
    if (plan_ptr) {
        printf("\nGuardrails:\n");
        printf("  Trials that never needed a cut:     %5.1f%%\n", 100.0 * (trials - gr_any_cut) / trials);
        printf("  Trials with a cut, money lasted:    %5.1f%%\n", 100.0 * gr_succ_with_cut / trials);
        printf("  Trials with a cut, money ran out:   %5.1f%%\n", 100.0 * (gr_any_cut - gr_succ_with_cut) / trials);
        printf("  Deepest cut reached:");
        for (int i = 0; i <= gr_n_steps; i++)
            printf("  %.0f%%: %.1f%%", i * c->guardrail_step_pct, 100.0 * gr_depth_count[i] / trials);
        printf("  (of trials)\n");
        if (gr_any_cut > 0) {
            int *yrs = malloc(gr_any_cut * sizeof(int)), k = 0;
            for (int t = 0; t < trials; t++) if (gr_years[t] > 0) yrs[k++] = gr_years[t];
            for (int i = 1; i < k; i++) {
                int key = yrs[i], j = i - 1;
                while (j >= 0 && yrs[j] > key) { yrs[j+1] = yrs[j]; j--; }
                yrs[j+1] = key;
            }
            printf("  Years spent with a cut, among trials that cut: median %d, p90 %d (of %d simulated years)\n",
                   yrs[k / 2], yrs[(int)(0.9 * (k - 1))], c->sim_years);
            free(yrs);
        }
        printf("  (Success above means meeting the REDUCED target while a cut is in force.)\n");
    }
    printf("\nPer-trial detail written to: %s\n", mc_csv_path);

    free(returns);
    free(roi_by_year);
    free(ending_totals);
    free(ran_out_years);
    free(sorted_totals);
    free(fail_years);
    free(gr_years);

    return 1;
}
