#include "retirement_sim.h"

/* CRA prescribed RRIF minimum withdrawal factors (Income Tax Regulation
 * s. 7308). Ages 65-70 use the formula 1/(90-age); ages 71-94 are a fixed
 * regulatory table, unchanged since a 2015 amendment; 95+ is a flat 20%.
 * Below 65, no prescribed minimum applies in this model.
 *
 * Index 0 = age 71, index 23 = age 94. */
static const double RRIF_TABLE_71_TO_94[] = {
    0.0528, 0.0540, 0.0553, 0.0567, 0.0582, 0.0598, 0.0617, 0.0636, /* 71-78 */
    0.0658, 0.0682, 0.0708, 0.0738, 0.0771, 0.0808, 0.0851, 0.0899, /* 79-86 */
    0.0955, 0.1021, 0.1099, 0.1192, 0.1306, 0.1449, 0.1634, 0.1879  /* 87-94 */
};

double rrif_min_factor(int age_on_jan1) {
    if (age_on_jan1 < 65) return 0.0;
    if (age_on_jan1 <= 70) return 1.0 / (90.0 - age_on_jan1);
    if (age_on_jan1 <= 94) return RRIF_TABLE_71_TO_94[age_on_jan1 - 71];
    return 0.20; /* 95 and over */
}
