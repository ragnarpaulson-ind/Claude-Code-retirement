#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include "retirement_sim.h"

void config_defaults(Config *c) {
    memset(c, 0, sizeof(*c));

    strcpy(c->a.name, "PersonA");
    strcpy(c->b.name, "PersonB");

    c->a.dob = (Date){1962, 1, 1};
    c->b.dob = (Date){1962, 1, 1};
    c->a.cpp_start = (Date){2027, 1, 1};
    c->b.cpp_start = (Date){2027, 1, 1};
    c->a.oas_start = (Date){2027, 1, 1};
    c->b.oas_start = (Date){2027, 1, 1};
    c->a.cpp_at_65 = 1200.0;
    c->a.oas_at_65 = 700.0;
    c->b.cpp_at_65 = 900.0;
    c->b.oas_at_65 = 700.0;

    c->retirement_date = (Date){2027, 1, 1};

    /* default sim start = today */
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    c->sim_start_date = (Date){lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday};
    c->sim_years = 40;

    c->tfsa_balance = 100000.0;
    c->tfsa_contribution_room = 100000.0;
    c->tfsa_annual_contribution_room_grant = 14000.0;
    c->person_a_registered_balance = 500000.0;
    c->person_b_registered_balance = 500000.0;

    c->roi_annual_pct = 5.0;
    c->personal_inflation_pct = 2.5;
    c->gov_inflation_pct = 2.0;

    c->target_net_monthly = 9000.0;
    c->target_gross_monthly = 50000.0 / 12.0 * 12.0; /* placeholder, monthly */
    c->target_gross_monthly = 50000.0;

    c->go_slow_year = 0;                 /* 0 = disabled */
    c->go_slow_resume_after_years = 0;

    /* A reasonable starter tax-bracket table (illustrative, not tax advice --
       edit these to match your actual jurisdiction/year in the config file). */
    c->n_brackets = 4;
    c->brackets[0] = (TaxBracket){0.0,      0.0000};
    c->brackets[1] = (TaxBracket){15000.0,  0.20};
    c->brackets[2] = (TaxBracket){50000.0,  0.30};
    c->brackets[3] = (TaxBracket){100000.0, 0.40};

    c->fed_age_credit_amount = 0.0;
    c->fed_age_credit_rate = 0.0;
    c->fed_age_credit_clawback_start = 1e18;
    c->prov_age_credit_amount = 0.0;
    c->prov_age_credit_rate = 0.0;
    c->prov_age_credit_clawback_start = 1e18;
    c->age_credit_clawback_rate = 0.15;

    c->n_events = 0;

    c->mc_trials = 10000;
    c->mc_returns_file[0] = '\0';
    c->mc_seed = 0;
    c->mc_run = 0;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = 0;
    return s;
}

int config_load(Config *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Could not open config file '%s'\n", path);
        return 0;
    }

    char line[512];
    int bracket_idx = 0;
    int have_brackets_from_file = 0;

    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (*l == 0 || *l == '#') continue;

        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(l);
        char *val = trim(eq + 1);

        if (strcmp(key, "person_a_name") == 0) { strncpy(c->a.name, val, sizeof(c->a.name)-1); }
        else if (strcmp(key, "person_b_name") == 0) { strncpy(c->b.name, val, sizeof(c->b.name)-1); }
        else if (strcmp(key, "person_a_dob") == 0) { parse_date(val, &c->a.dob); }
        else if (strcmp(key, "person_b_dob") == 0) { parse_date(val, &c->b.dob); }
        else if (strcmp(key, "person_a_cpp_start") == 0) { parse_date(val, &c->a.cpp_start); }
        else if (strcmp(key, "person_b_cpp_start") == 0) { parse_date(val, &c->b.cpp_start); }
        else if (strcmp(key, "person_a_oas_start") == 0) { parse_date(val, &c->a.oas_start); }
        else if (strcmp(key, "person_b_oas_start") == 0) { parse_date(val, &c->b.oas_start); }
        else if (strcmp(key, "person_a_cpp_at_65") == 0) { c->a.cpp_at_65 = atof(val); }
        else if (strcmp(key, "person_a_oas_at_65") == 0) { c->a.oas_at_65 = atof(val); }
        else if (strcmp(key, "person_b_cpp_at_65") == 0) { c->b.cpp_at_65 = atof(val); }
        else if (strcmp(key, "person_b_oas_at_65") == 0) { c->b.oas_at_65 = atof(val); }

        else if (strcmp(key, "retirement_date") == 0) { parse_date(val, &c->retirement_date); }
        else if (strcmp(key, "sim_start_date") == 0) { parse_date(val, &c->sim_start_date); }
        else if (strcmp(key, "rent_start_date") == 0) { parse_date(val, &c->rent_start_date); }
        else if (strcmp(key, "rent_monthly") == 0) { c->rent_monthly = atof(val); }
        else if (strcmp(key, "sim_years") == 0) { c->sim_years = atoi(val); }

        else if (strcmp(key, "tfsa_balance") == 0) { c->tfsa_balance = atof(val); }
        else if (strcmp(key, "tfsa_contribution_room") == 0) { c->tfsa_contribution_room = atof(val); }
        else if (strcmp(key, "tfsa_annual_contribution_room_grant") == 0) { c->tfsa_annual_contribution_room_grant = atof(val); }
        else if (strcmp(key, "person_a_registered_balance") == 0) { c->person_a_registered_balance = atof(val); }
        else if (strcmp(key, "person_b_registered_balance") == 0) { c->person_b_registered_balance = atof(val); }
        else if (strcmp(key, "person_a_nonreg_balance") == 0) { c->person_a_nonreg_balance = atof(val); }
        else if (strcmp(key, "person_b_nonreg_balance") == 0) { c->person_b_nonreg_balance = atof(val); }   

        else if (strcmp(key, "roi_annual_pct") == 0) { c->roi_annual_pct = atof(val); }
        else if (strcmp(key, "personal_inflation_pct") == 0) { c->personal_inflation_pct = atof(val); }
        else if (strcmp(key, "gov_inflation_pct") == 0) { c->gov_inflation_pct = atof(val); }

        else if (strcmp(key, "target_net_monthly") == 0) { c->target_net_monthly = atof(val); }
        else if (strcmp(key, "target_gross_monthly") == 0) { c->target_gross_monthly = atof(val); }

        else if (strcmp(key, "go_slow_year") == 0) { c->go_slow_year = atoi(val); }
        else if (strcmp(key, "go_slow_resume_after_years") == 0) { c->go_slow_resume_after_years = atoi(val); }

        else if (strcmp(key, "tax_bracket") == 0) {
            if (!have_brackets_from_file) { bracket_idx = 0; have_brackets_from_file = 1; }
            double lower, rate;
            if (sscanf(val, "%lf,%lf", &lower, &rate) == 2 && bracket_idx < MAX_TAX_BRACKETS) {
                c->brackets[bracket_idx].lower_limit = lower;
                c->brackets[bracket_idx].marginal_rate = rate;
                bracket_idx++;
                c->n_brackets = bracket_idx;
            }
        }

        else if (strcmp(key, "fed_age_credit_amount") == 0) { c->fed_age_credit_amount = atof(val); }
        else if (strcmp(key, "fed_age_credit_rate") == 0) { c->fed_age_credit_rate = atof(val); }
        else if (strcmp(key, "fed_age_credit_clawback_start") == 0) { c->fed_age_credit_clawback_start = atof(val); }
        else if (strcmp(key, "prov_age_credit_amount") == 0) { c->prov_age_credit_amount = atof(val); }
        else if (strcmp(key, "prov_age_credit_rate") == 0) { c->prov_age_credit_rate = atof(val); }
        else if (strcmp(key, "prov_age_credit_clawback_start") == 0) { c->prov_age_credit_clawback_start = atof(val); }
        else if (strcmp(key, "age_credit_clawback_rate") == 0) { c->age_credit_clawback_rate = atof(val); }
        else if (strcmp(key, "events_file") == 0) { events_load(c, val); }

        else if (strcmp(key, "mc_trials") == 0) { c->mc_trials = atoi(val); }
        else if (strcmp(key, "mc_returns_file") == 0) { strncpy(c->mc_returns_file, val, sizeof(c->mc_returns_file)-1); }
        else if (strcmp(key, "mc_seed") == 0) { c->mc_seed = (unsigned int)strtoul(val, NULL, 10); }
        else {
            fprintf(stderr, "Warning: unknown config key '%s' (ignored)\n", key);
        }
    }

    fclose(f);
    return 1;
}

int events_load(Config *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Could not open events file '%s'\n", path);
        return 0;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (*l == 0 || *l == '#') continue;
        if (c->n_events >= MAX_EVENTS) break;

        char datestr[32] = {0};
        double amount = 0;
        char note[128] = {0};

        char *p = l;
        char *comma1 = strchr(p, ',');
        if (!comma1) continue;
        *comma1 = 0;
        strncpy(datestr, p, sizeof(datestr)-1);

        char *rest = comma1 + 1;
        char *comma2 = strchr(rest, ',');
        if (comma2) { *comma2 = 0; amount = atof(rest); strncpy(note, comma2+1, sizeof(note)-1); }
        else { amount = atof(rest); }

        OneTimeEvent ev;
        if (!parse_date(datestr, &ev.date)) continue;
        ev.amount = amount;
        strncpy(ev.note, trim(note), sizeof(ev.note)-1);

        c->events[c->n_events++] = ev;
    }
    fclose(f);
    return 1;
}

static double parse_scenario_double(const char *s) { return atof(s); }

void config_apply_overrides(Config *c, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--roi") == 0 && i + 1 < argc) c->roi_annual_pct = parse_scenario_double(argv[++i]);
        else if (strcmp(argv[i], "--inflation") == 0 && i + 1 < argc) c->personal_inflation_pct = parse_scenario_double(argv[++i]);
        else if (strcmp(argv[i], "--gov-inflation") == 0 && i + 1 < argc) c->gov_inflation_pct = parse_scenario_double(argv[++i]);
        else if (strcmp(argv[i], "--target-net") == 0 && i + 1 < argc) c->target_net_monthly = parse_scenario_double(argv[++i]);
        else if (strcmp(argv[i], "--target-gross") == 0 && i + 1 < argc) c->target_gross_monthly = parse_scenario_double(argv[++i]);
        else if (strcmp(argv[i], "--years") == 0 && i + 1 < argc) c->sim_years = atoi(argv[++i]);
        else if (strcmp(argv[i], "--retire-date") == 0 && i + 1 < argc) parse_date(argv[++i], &c->retirement_date);
        else if (strcmp(argv[i], "--tfsa") == 0 && i + 1 < argc) c->tfsa_balance = parse_scenario_double(argv[++i]);
        else if (strcmp(argv[i], "--registered-a") == 0 && i + 1 < argc) c->person_a_registered_balance = parse_scenario_double(argv[++i]);
        else if (strcmp(argv[i], "--registered-b") == 0 && i + 1 < argc) c->person_b_registered_balance = parse_scenario_double(argv[++i]);
	    else if (strcmp(argv[i], "--monte-carlo") == 0) c->mc_run = 1;
        else if (strcmp(argv[i], "--mc-in-turn") == 0) c->mc_in_turn = 1;
    }
}
