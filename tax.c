#include "retirement_sim.h"

/* Generic progressive marginal-rate tax calculator: given a sorted (ascending)
 * bracket table of {lower_limit, marginal_rate}, computes tax owed on
 * annual_taxable_income. Brackets scale with gov_inflate_factor, mirroring
 * the spreadsheet's practice of inflating bracket thresholds every year.
 *
 * This replaces the spreadsheet's nested IF()-per-bracket formula (which was
 * hand-limited to 4 brackets in most year-blocks) with a loop over however
 * many brackets the config file defines -- add a bracket, get correct tax,
 * no formula surgery required. */
double tax_owed_annual(const Config *c, double annual_taxable_income, double gov_inflate_factor) {
    if (annual_taxable_income <= 0 || c->n_brackets == 0) return 0.0;

    double tax = 0.0;
    for (int i = 0; i < c->n_brackets; i++) {
        double lower = c->brackets[i].lower_limit * gov_inflate_factor;
        double upper = (i + 1 < c->n_brackets)
                        ? c->brackets[i + 1].lower_limit * gov_inflate_factor
                        : 1e18;
        if (annual_taxable_income <= lower) break;
        double taxable_in_bracket = (annual_taxable_income < upper ? annual_taxable_income : upper) - lower;
        if (taxable_in_bracket > 0) tax += taxable_in_bracket * c->brackets[i].marginal_rate;
    }
    return tax;
}

/* Age credit (federal + provincial), only for taxpayers 65+, clawed back
 * 15%-style above an income threshold. Floored at zero -- unlike the source
 * spreadsheet's formula, which could mathematically go negative and reduce
 * tax further at high income. That's a deliberate correction, flagged in
 * the README. */
double age_credit_annual(const Config *c, double annual_taxable_income, double gov_inflate_factor) {
    if (annual_taxable_income <= 0) return 0.0;

    double fed_limit = c->fed_age_credit_clawback_start * gov_inflate_factor;
    double fed_credit = (c->fed_age_credit_amount * gov_inflate_factor
                          - (annual_taxable_income > fed_limit ? (annual_taxable_income - fed_limit) : 0.0) * c->age_credit_clawback_rate)
                         * c->fed_age_credit_rate;
    if (fed_credit < 0) fed_credit = 0;

    double prov_limit = c->prov_age_credit_clawback_start * gov_inflate_factor;
    double prov_credit = (c->prov_age_credit_amount * gov_inflate_factor
                           - (annual_taxable_income > prov_limit ? (annual_taxable_income - prov_limit) : 0.0) * c->age_credit_clawback_rate)
                          * c->prov_age_credit_rate;
    if (prov_credit < 0) prov_credit = 0;

    return fed_credit + prov_credit;
}
