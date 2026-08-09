#include <stdio.h>
#include <string.h>
#include <math.h>
#include "retirement_sim.h"

static double cpp_adjustment_factor(Date cpp_start, Date age65) {
    if (date_lt(cpp_start, age65)) {
        int months_early = months_between(cpp_start, age65);
        return 1.0 - months_early * 0.006;   /* 0.6%/month early reduction */
    } else {
        int months_late = months_between(age65, cpp_start);
        return 1.0 + months_late * 0.007;    /* 0.7%/month deferral bonus */
    }
}

static double oas_adjustment_factor(Date oas_start, Date age65) {
    if (date_lt(age65, oas_start)) {
        int months_late = months_between(age65, oas_start);
        return 1.0 + months_late * 0.006;    /* 0.6%/month deferral bonus, no early option */
    }
    return 1.0;
}

static int is_age_65_plus(Date current, Date age65) {
    return date_ge(current, age65);
}

/* Computes tax, age credit, household gross, and net-of-tax for a given pair
 * of retirement-fund draws. Pulled out into its own function because the
 * gross-up solve below needs to re-run this same calculation several times
 * for different candidate draw amounts. */
static void compute_net(const Config *c, Date cur, Date age65_a, Date age65_b, double gov_inflate,
                         double fixed_a, double fixed_b, double draw_a, double draw_b,
                         double *out_tax_a, double *out_tax_b,
                         double *out_credit_a, double *out_credit_b,
                         double *out_household_gross, double *out_net_of_tax) {
    double taxable_a = draw_a + fixed_a;
    double taxable_b = draw_b + fixed_b;

    double tax_a = -tax_owed_annual(c, taxable_a * 12.0, gov_inflate) / 12.0;
    double tax_b = -tax_owed_annual(c, taxable_b * 12.0, gov_inflate) / 12.0;
    double credit_a = is_age_65_plus(cur, age65_a) ? age_credit_annual(c, taxable_a * 12.0, gov_inflate) / 12.0 : 0.0;
    double credit_b = is_age_65_plus(cur, age65_b) ? age_credit_annual(c, taxable_b * 12.0, gov_inflate) / 12.0 : 0.0;

    *out_tax_a = tax_a;
    *out_tax_b = tax_b;
    *out_credit_a = credit_a;
    *out_credit_b = credit_b;
    *out_household_gross = taxable_a + taxable_b;
    *out_net_of_tax = *out_household_gross + tax_a + tax_b + credit_a + credit_b;
}

int run_simulation_ex(const Config *c, const char *csv_path, const double *roi_override,
                       int quiet, SimResult *result_out) {
    FILE *csv = NULL;
    if (csv_path) {
        csv = fopen(csv_path, "w");
        if (!csv) { fprintf(stderr, "Could not open output file '%s'\n", csv_path); return 0; }

        fprintf(csv, "date,reg_a_wd, reg_b_wd,household_gross,tax_a,tax_b,age_credit_a,age_credit_b,"
                     "net_of_tax,non_reg_shortfall_draw,tfsa_topup,jan_nonreg_to_tfsa_sweep,net_takehome,tfsa_balance,"
                     "registered_balance_a,non-registered_balance_a,registered_balance_b,non-registered_balance_b,rrif_min_a,rrif_min_b,event\n");
    }

    Date age65_a, age65_b;
    date_add_years(c->a.dob, 65, &age65_a);
    date_add_years(c->b.dob, 65, &age65_b);

    double cpp_factor_a = cpp_adjustment_factor(c->a.cpp_start, age65_a);
    double cpp_factor_b = cpp_adjustment_factor(c->b.cpp_start, age65_b);
    double oas_factor_a = oas_adjustment_factor(c->a.oas_start, age65_a);
    double oas_factor_b = oas_adjustment_factor(c->b.oas_start, age65_b);

    double tfsa_balance = c->tfsa_balance;
    double tfsa_contribution_room = c->tfsa_contribution_room;
    double tfsa_contribution_room_change = 0.0;
    double jan_sweep_amount = 0.0;  /* non-registered -> TFSA lump sum swept this January, for CSV visibility */
    double fund_a = c->person_a_registered_balance;
    double fund_b = c->person_b_registered_balance;
    double fund_a_nonreg = c->person_a_nonreg_balance;
    double fund_b_nonreg = c->person_b_nonreg_balance;

    /* RRIF minimum withdrawal: recomputed each January 1 from that day's
       balance and that day's age, then held flat (spread evenly across the
       year) until the next January 1. */
    double rrif_monthly_min_a = fund_a * rrif_min_factor(c->sim_start_date.year - c->a.dob.year) / 12.0;
    double rrif_monthly_min_b = fund_b * rrif_min_factor(c->sim_start_date.year - c->b.dob.year) / 12.0;

    double gov_inflate = 1.0;
    double personal_inflate = 1.0;

    int prev_year = c->sim_start_date.year;

    int tfsa_depleted_reported = 0;
    int fund_a_depleted_reported = 0, fund_b_depleted_reported = 0;
    Date tfsa_depleted_date = {0,0,0}, fund_a_depleted_date = {0,0,0}, fund_b_depleted_date = {0,0,0};
    int household_shortfall_reported = 0;
    Date household_shortfall_date = {0,0,0};
    int household_shortfall_months = 0;
    int money_exhausted_reported = 0;
    Date money_exhausted_date = {0,0,0};
    int money_exhausted_months = 0;

    double annual_net_takehome = 0.0;
    int total_months = c->sim_years * 12;
    int last_month_processed = c->sim_start_date.month;

    double rent_monthly = c->rent_monthly;

    for (int m = 0; m < total_months; m++) {
        int year = c->sim_start_date.year + (c->sim_start_date.month - 1 + m) / 12;
        int month = (c->sim_start_date.month - 1 + m) % 12 + 1;
        Date cur = first_of_month(year, month);
        last_month_processed = month;
        jan_sweep_amount = 0.0;

        /* Which annual ROI rate applies this month: Monte Carlo supplies a
           different rate per simulated calendar year (one draw from the
           historical return series); otherwise it's the flat configured
           rate every year. */
        double current_roi_pct = roi_override ? roi_override[year - c->sim_start_date.year] : c->roi_annual_pct;

        if (year != prev_year) {
            /* Print the true Dec 31 closing balances FIRST, before any of
               this new year's January 1 activity (RRIF re-baseline, TFSA
               room grant, nonreg->TFSA sweep) mutates them. Previously this
               print ran after that activity, so the reported "close" figures
               were actually next January's post-sweep balances, not the
               real prior-year closing balances. */
            int months_in_prev_year = (prev_year == c->sim_start_date.year)
                                       ? (13 - c->sim_start_date.month)
                                       : 12;
            if (!quiet) {
                if (months_in_prev_year < 12) {
                    printf("Year %4d close (partial, %d mo): TFSA $%.0f | Registered/Non A $%.0f/%.0f | Registered/Non B $%.0f/%.0f | Net take-home YTD $%.0f\n",
                           prev_year, months_in_prev_year, tfsa_balance, fund_a, fund_a_nonreg, fund_b, fund_b_nonreg, annual_net_takehome);
                } else {
                    printf("Year %4d close: TFSA $%.0f | Registered/Non A $%.0f/%.0f | Registered/Non B $%.0f/%.0f | Net take-home YTD $%.0f\n",
                           prev_year, tfsa_balance, fund_a, fund_a_nonreg, fund_b, fund_b_nonreg, annual_net_takehome);
                }
            }
            annual_net_takehome = 0.0;
            prev_year = year;

            gov_inflate *= (1.0 + c->gov_inflation_pct / 100.0);
            int in_go_slow = c->go_slow_year > 0 &&
                              year >= c->go_slow_year &&
                              year <= c->go_slow_year + c->go_slow_resume_after_years;
            if (!in_go_slow) personal_inflate *= (1.0 + c->personal_inflation_pct / 100.0);
	        /* monthly rent increases by inflation eery year */
	        rent_monthly *= (1.0 + c->personal_inflation_pct / 100.0);

            /* New January 1: re-baseline the RRIF minimum from this year's
               opening balance and age. */
            rrif_monthly_min_a = fund_a * rrif_min_factor(year - c->a.dob.year) / 12.0;
            rrif_monthly_min_b = fund_b * rrif_min_factor(year - c->b.dob.year) / 12.0;
            /* New TFSA contribution room */
            tfsa_contribution_room += c->tfsa_annual_contribution_room_grant;
            tfsa_contribution_room += tfsa_contribution_room_change;
            tfsa_contribution_room_change = 0.0;
            if (fund_a_nonreg + fund_b_nonreg > 0) {
                double total_nonreg = fund_a_nonreg + fund_b_nonreg;
                if (total_nonreg > tfsa_contribution_room) {
                    double tfsa_topup_a = tfsa_contribution_room * (fund_a_nonreg / total_nonreg);
                    double tfsa_topup_b = tfsa_contribution_room * (fund_b_nonreg / total_nonreg);
                    fund_a_nonreg -= tfsa_topup_a;
                    fund_b_nonreg -= tfsa_topup_b;
                    tfsa_balance += tfsa_contribution_room;
                    jan_sweep_amount = tfsa_contribution_room;
                    tfsa_contribution_room = 0.0;
                } else {
                    tfsa_balance += total_nonreg;
                    tfsa_contribution_room -= total_nonreg;
                    jan_sweep_amount = total_nonreg;
                    fund_a_nonreg = 0.0;
                    fund_b_nonreg = 0.0;    
                }
            }
        }

        int retired = date_ge(cur, c->retirement_date);

        double cpp_a = date_ge(cur, c->a.cpp_start) ? c->a.cpp_at_65 * cpp_factor_a * gov_inflate : 0.0;
        double cpp_b = date_ge(cur, c->b.cpp_start) ? c->b.cpp_at_65 * cpp_factor_b * gov_inflate : 0.0;
        double oas_a = date_ge(cur, c->a.oas_start) ? c->a.oas_at_65 * oas_factor_a * gov_inflate : 0.0;
        double oas_b = date_ge(cur, c->b.oas_start) ? c->b.oas_at_65 * oas_factor_b * gov_inflate : 0.0;

        double non_reg_income_a = 0.0, non_reg_income_b = 0.0;
        if (fund_a_nonreg > 0) {
            non_reg_income_a = fund_a_nonreg * current_roi_pct / 100.0 / 12.0;
        }
        if (fund_b_nonreg > 0) {
            non_reg_income_b = fund_b_nonreg * current_roi_pct / 100.0 / 12.0;
        }

        double fixed_a = cpp_a + oas_a + non_reg_income_a;
        double fixed_b = cpp_b + oas_b + non_reg_income_b;

        double target_gross_pp = retired ? c->target_gross_monthly * personal_inflate : 0.0;
        double base_draw_a = retired ? (target_gross_pp - fixed_a) : 0.0;
        double base_draw_b = retired ? (target_gross_pp - fixed_b) : 0.0;
        if (base_draw_a < 0) base_draw_a = 0;
        if (base_draw_b < 0) base_draw_b = 0;

        /* RRIF minimum withdrawal is a hard floor, independent of the
           spending target -- applies whether or not it's actually needed
           this month. Currently the target-driven draw is comfortably above
           it, so this has no visible effect; it starts to matter once a
           non-registered account can absorb spending instead, since without
           this floor the sim would otherwise draw the registered accounts
           down slower than the law requires. */
        if (base_draw_a < rrif_monthly_min_a) base_draw_a = rrif_monthly_min_a;
        if (base_draw_b < rrif_monthly_min_b) base_draw_b = rrif_monthly_min_b;

        /* Physically, a registered account can only pay out what it holds --
           but real pension-income splitting (CRA form T1032) lets a couple
           reallocate up to 50% of eligible pension/RRIF income to the other
           spouse's tax return, regardless of which account the cash actually
           came from. So once one person's account runs dry, the other
           account supplies the cash while tax attribution stays exactly as
           if each had withdrawn their own share. This has to happen BEFORE
           net_of_tax/shortfall are computed below -- otherwise the shortfall
           (and the TFSA/non-reg top-up sized against it) gets based on
           phantom income the accounts can't actually pay, and once a
           registered account is truly dry the TFSA silently stops being
           drawn down to cover the gap. */
        double fund_a_avail = fund_a, fund_b_avail = fund_b;
        double cash_a = base_draw_a < fund_a_avail ? base_draw_a : fund_a_avail;
        double cash_b = base_draw_b < fund_b_avail ? base_draw_b : fund_b_avail;
        double short_a = base_draw_a - cash_a;
        double short_b = base_draw_b - cash_b;

        double from_b_for_a = short_a < (fund_b_avail - cash_b) ? short_a : (fund_b_avail - cash_b);
        if (from_b_for_a < 0) from_b_for_a = 0;
        cash_b += from_b_for_a;
        short_a -= from_b_for_a;

        double from_a_for_b = short_b < (fund_a_avail - cash_a) ? short_b : (fund_a_avail - cash_a);
        if (from_a_for_b < 0) from_a_for_b = 0;
        cash_a += from_a_for_b;
        short_b -= from_a_for_b;

        double draw_a, draw_b;
        if (short_a + short_b > 0.01) {
            /* Both accounts combined can't cover even the base/RRIF-floor
               draw -- a real plan failure at the registered-fund level, not
               just one account running dry. Taxable income has to match the
               cash that genuinely exists. */
            draw_a = cash_a;
            draw_b = cash_b;
            if (!household_shortfall_reported) { household_shortfall_reported = 1; household_shortfall_date = cur; }
            household_shortfall_months++;
        } else {
            draw_a = base_draw_a;
            draw_b = base_draw_b;
        }
        /* Registered capacity NOT used by the base draw -- still available
           later this month if TFSA + non-reg can't cover the rest of the
           shortfall. */
        double reg_leftover_a = fund_a_avail - cash_a;
        double reg_leftover_b = fund_b_avail - cash_b;

        double tax_a, tax_b, credit_a, credit_b, household_gross, net_of_tax;
        compute_net(c, cur, age65_a, age65_b, gov_inflate, fixed_a, fixed_b, draw_a, draw_b,
                    &tax_a, &tax_b, &credit_a, &credit_b, &household_gross, &net_of_tax);

        double target_net_effective = c->target_net_monthly * personal_inflate;
	    int renting = date_ge(cur, c->rent_start_date) && rent_monthly > 0.01;
	    if (renting) target_net_effective += rent_monthly;
        double shortfall = retired ? (target_net_effective - net_of_tax) : 0.0;

        /* first try to cover the shortfall with the non registered accounts, take from each account 
        equally if possible */
        double non_reg_draw_total = 0.0;
        if (fund_a_nonreg + fund_b_nonreg > 0) {
            double non_reg_total = fund_a_nonreg + fund_b_nonreg;
            double non_reg_draw_a = shortfall * (fund_a_nonreg / non_reg_total);
            double non_reg_draw_b = shortfall * (fund_b_nonreg / non_reg_total);

            if (non_reg_draw_a > fund_a_nonreg) {
                non_reg_draw_b += (non_reg_draw_a - fund_a_nonreg);
                non_reg_draw_a = fund_a_nonreg;
            }
            if (non_reg_draw_b > fund_b_nonreg) {
                non_reg_draw_b = fund_b_nonreg;
            }

            fund_a_nonreg -= non_reg_draw_a;
            fund_b_nonreg -= non_reg_draw_b;

            non_reg_draw_total = non_reg_draw_a + non_reg_draw_b;
            shortfall -= non_reg_draw_total;
        }
        double tfsa_topup = shortfall;
        if (tfsa_topup < 0) tfsa_topup = 0;
        if (tfsa_topup > tfsa_balance) tfsa_topup = tfsa_balance;

        /* Whatever the TFSA can't cover has to come from an additional,
           TAXABLE registered-fund draw -- but only up to whatever capacity
           is genuinely still left (reg_leftover_a/b) after the base draw
           above. That draw's after-tax proceeds depend on its own size (it
           changes taxable income, which changes tax, which changes the
           after-tax proceeds) -- so instead of guessing a threshold, solve
           for it: start from the shortfall amount, see how much it actually
           nets after tax, and correct the guess by the residual, clamping
           to available capacity each pass. Tax is piecewise-linear in
           income, so this converges in well under 25 passes to sub-cent
           precision (or clamps at the capacity ceiling if that's reached
           first). */
        double remaining = shortfall - tfsa_topup;
        double reg_leftover_total = reg_leftover_a + reg_leftover_b;
        if (retired && remaining > 0.01 && reg_leftover_total > 0.01) {
            double net_of_tax_base = net_of_tax;
            double extra = remaining < reg_leftover_total ? remaining : reg_leftover_total;
            double extra_a = 0.0, extra_b = 0.0;

            for (int iter = 0; iter < 25; iter++) {
                if (extra < 0) extra = 0;
                if (extra > reg_leftover_total) extra = reg_leftover_total;
                extra_a = extra * (reg_leftover_a / reg_leftover_total);
                extra_b = extra * (reg_leftover_b / reg_leftover_total);

                double ta, tb, ca, cb, hg, net2;
                compute_net(c, cur, age65_a, age65_b, gov_inflate, fixed_a, fixed_b,
                            draw_a + extra_a, draw_b + extra_b, &ta, &tb, &ca, &cb, &hg, &net2);
                double net_gain = net2 - net_of_tax_base;
                double diff = remaining - net_gain;
                if (fabs(diff) < 0.01) { extra += diff; break; }
                extra += diff;
            }
            if (extra < 0) extra = 0;
            if (extra > reg_leftover_total) extra = reg_leftover_total;
            extra_a = extra * (reg_leftover_a / reg_leftover_total);
            extra_b = extra * (reg_leftover_b / reg_leftover_total);

            draw_a += extra_a;
            draw_b += extra_b;
            cash_a += extra_a;
            cash_b += extra_b;
            compute_net(c, cur, age65_a, age65_b, gov_inflate, fixed_a, fixed_b, draw_a, draw_b,
                        &tax_a, &tax_b, &credit_a, &credit_b, &household_gross, &net_of_tax);
        }

        double net_takehome = net_of_tax + tfsa_topup + (non_reg_draw_total > 0 ? non_reg_draw_total : 0.0);
        annual_net_takehome += net_takehome;

        /* True depletion check: after registered (base + any extra),
           non-reg, and TFSA have all been drawn as far as they can go, is
           the household still short of its target this month? If so, every
           source of money is genuinely exhausted, not just one account --
           report the first month/year this happens. */
        if (retired && (target_net_effective - net_takehome) > 0.01) {
            if (!money_exhausted_reported) { money_exhausted_reported = 1; money_exhausted_date = cur; }
            money_exhausted_months++;
        }

        /* one-time events this month */
        double event_amount = 0.0;
        char event_note[256] = "";
        for (int e = 0; e < c->n_events; e++) {
            if (c->events[e].date.year == year && c->events[e].date.month == month) {
                event_amount += c->events[e].amount;
                if (event_note[0]) strncat(event_note, "; ", sizeof(event_note) - strlen(event_note) - 1);
                strncat(event_note, c->events[e].note, sizeof(event_note) - strlen(event_note) - 1);
            }
        }

        tfsa_balance = tfsa_balance * (1.0 + current_roi_pct / 1200.0) - tfsa_topup;
        tfsa_contribution_room_change += tfsa_topup;

        /* One-time events currently split evenly across both non-registered
           accounts first, then, if event is negative and non-registered are empty, remove
           from registered accounts. This is a bit arbitrary, but the idea is that
           windfalls like a home sale should route there instead, since
           unlike these registered accounts it isn't fully taxable on
           withdrawal. */
        if (event_amount < 0) {
            double nonreg_total = fund_a_nonreg + fund_b_nonreg;
            if (nonreg_total > 0) {
                double nonreg_draw_a = event_amount * (fund_a_nonreg / nonreg_total);
                double nonreg_draw_b = event_amount * (fund_b_nonreg / nonreg_total);

                if (nonreg_draw_a < -fund_a_nonreg) {
                    nonreg_draw_b += (nonreg_draw_a + fund_a_nonreg);
                    nonreg_draw_a = -fund_a_nonreg;
                }
                if (nonreg_draw_b < -fund_b_nonreg) {
                    nonreg_draw_b = -fund_b_nonreg;
                }

                fund_a_nonreg += nonreg_draw_a;
                fund_b_nonreg += nonreg_draw_b;

                event_amount -= (nonreg_draw_a + nonreg_draw_b);
            }
        } else if (event_amount > 0) {
            fund_a_nonreg += event_amount / 2.0;
            fund_b_nonreg += event_amount / 2.0;
            event_amount = 0.0;
        }
        fund_a = fund_a * (1.0 + current_roi_pct / 1200.0) - cash_a + event_amount / 2.0;
        fund_b = fund_b * (1.0 + current_roi_pct / 1200.0) - cash_b + event_amount / 2.0;

        if (tfsa_balance < 0) {
            if (!tfsa_depleted_reported) { tfsa_depleted_reported = 1; tfsa_depleted_date = cur; }
            tfsa_balance = 0;
        }
        if (fund_a < 0) {
            if (!fund_a_depleted_reported) { fund_a_depleted_reported = 1; fund_a_depleted_date = cur; }
            fund_a = 0;
        }
        if (fund_b < 0) {
            if (!fund_b_depleted_reported) { fund_b_depleted_reported = 1; fund_b_depleted_date = cur; }
            fund_b = 0;
        }

        if (csv) {
            fprintf(csv, "%04d-%02d-01,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%s\n",
                    year, month, draw_a, draw_b,
                    household_gross, tax_a, tax_b, credit_a, credit_b,
                    net_of_tax, non_reg_draw_total, tfsa_topup, jan_sweep_amount, net_takehome, tfsa_balance,
                    fund_a, fund_a_nonreg, fund_b, fund_b_nonreg, rrif_monthly_min_a, rrif_monthly_min_b, event_note);
        }
    }

    if (!quiet) {
        int months_in_final_year = (prev_year == c->sim_start_date.year)
                                    ? (last_month_processed - c->sim_start_date.month + 1)
                                    : last_month_processed;
        if (months_in_final_year < 12) {
            printf("Year %4d close (partial, %d mo): TFSA $%.0f | Registered A/Non-Registered A $%.0f/%.0f | Registered B/Non-Registered B $%.0f/%.0f | Net take-home YTD $%.0f\n",
                   prev_year, months_in_final_year, tfsa_balance, fund_a, fund_a_nonreg, fund_b, fund_b_nonreg, annual_net_takehome);
        } else {
            printf("Year %4d close: TFSA $%.0f | Registered A/Non-Registered A $%.0f/%.0f | Registered B/Non-Registered B $%.0f/%.0f | Net take-home YTD $%.0f\n",
                   prev_year, tfsa_balance, fund_a, fund_a_nonreg, fund_b, fund_b_nonreg, annual_net_takehome);
        }
    }

    if (csv) fclose(csv);

    if (result_out) {
        result_out->money_ran_out = money_exhausted_reported;
        result_out->money_ran_out_date = money_exhausted_date;
        result_out->ending_tfsa = tfsa_balance;
        result_out->ending_registered_a = fund_a;
        result_out->ending_registered_b = fund_b;
        result_out->ending_nonreg_a = fund_a_nonreg;
        result_out->ending_nonreg_b = fund_b_nonreg;
        result_out->ending_total = tfsa_balance + fund_a + fund_b + fund_a_nonreg + fund_b_nonreg;
    }

    if (!quiet) {
        printf("\n=== Summary ===\n");
        printf("Simulated %d years from %04d-%02d-01.\n", c->sim_years, c->sim_start_date.year, c->sim_start_date.month);
        printf("Ending TFSA balance:          $%.0f\n", tfsa_balance);
        printf("Ending registered balance A:  $%.0f\n", fund_a);
        printf("Ending non-registered balance A: $%.0f\n", fund_a_nonreg);
        printf("Ending registered balance B:  $%.0f\n", fund_b);
        printf("Ending non-registered balance B: $%.0f\n", fund_b_nonreg);
        printf("Ending TFSA Contribution Room: $%.0f\n", tfsa_contribution_room);
        if (tfsa_depleted_reported)
            printf("NOTE: TFSA was exhausted in %04d-%02d.\n", tfsa_depleted_date.year, tfsa_depleted_date.month);
        if (fund_a_depleted_reported)
            printf("NOTE: Person A's registered account was exhausted in %04d-%02d (draws continued, sourced from Person B's account).\n",
                   fund_a_depleted_date.year, fund_a_depleted_date.month);
        if (fund_b_depleted_reported)
            printf("NOTE: Person B's registered account was exhausted in %04d-%02d (draws continued, sourced from Person A's account).\n",
                   fund_b_depleted_date.year, fund_b_depleted_date.month);
        if (household_shortfall_reported)
            printf("WARNING: registered funds combined were insufficient to meet the base/RRIF-minimum draw in %d month(s), starting %04d-%02d.\n",
                   household_shortfall_months, household_shortfall_date.year, household_shortfall_date.month);
        if (money_exhausted_reported)
            printf("WARNING: money ran out -- registered funds, non-registered funds, and TFSA combined could not meet the target income in %d month(s), starting %04d-%02d.\n",
                   money_exhausted_months, money_exhausted_date.year, money_exhausted_date.month);
        if (!tfsa_depleted_reported && !fund_a_depleted_reported && !fund_b_depleted_reported)
            printf("No account was depleted over the simulated horizon.\n");
        if (csv_path) printf("Full month-by-month detail written to: %s\n", csv_path);
    }

    return 1;
}

/* Backward-compatible entry point: unchanged behaviour for existing callers. */
int run_simulation(const Config *c, const char *csv_path) {
    return run_simulation_ex(c, csv_path, NULL, 0, NULL);
}
