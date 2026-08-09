#ifndef DATEUTIL_H
#define DATEUTIL_H

#include <stdio.h>

typedef struct {
    int year;
    int month;   /* 1-12 */
    int day;     /* 1-31 */
} Date;

/* Days since 0000-03-01, using Howard Hinnant's civil_from_days algorithm
 * (public domain). Lets us compare/subtract dates without relying on
 * libc's time_t range (which can choke on dates far in the future/past). */
static inline long date_to_days(Date d) {
    int y = d.year;
    int m = d.month;
    int day = d.day;
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);              /* [0, 399] */
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1; /* [0,365] */
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;   /* [0, 146096] */
    return era * 146097 + (long)doe - 719468;
}

/* -1 if a<b, 0 if equal, 1 if a>b */
static inline int date_cmp(Date a, Date b) {
    long da = date_to_days(a), db = date_to_days(b);
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static inline int date_lt(Date a, Date b) { return date_cmp(a, b) < 0; }
static inline int date_ge(Date a, Date b) { return date_cmp(a, b) >= 0; }

/* Whole months between two dates, matching Excel's DATEDIF(a,b,"m"):
 * counts complete months, so a partial trailing month doesn't count. */
static inline int months_between(Date a, Date b) {
    int months = (b.year - a.year) * 12 + (b.month - a.month);
    if (b.day < a.day) months -= 1;
    if (months < 0) months = 0;
    return months;
}

/* The first-of-month Date used for a given simulated (year, month). */
static inline Date first_of_month(int year, int month) {
    Date d = { year, month, 1 };
    return d;
}

/* Parses "YYYY-MM-DD". Returns 1 on success, 0 on failure. */
static inline int parse_date(const char *s, Date *out) {
    int y, m, d;
    if (sscanf(s, "%d-%d-%d", &y, &m, &d) != 3) return 0;
    if (m < 1 || m > 12 || d < 1 || d > 31) return 0;
    out->year = y; out->month = m; out->day = d;
    return 1;
}

static inline void date_add_years(Date base, int years, Date *out) {
    out->year = base.year + years;
    out->month = base.month;
    out->day = base.day;
}

#endif
