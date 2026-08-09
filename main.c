#include <stdio.h>
#include <string.h>
#include "retirement_sim.h"

static void print_usage(const char *prog) {
    printf("Usage: %s --config <file.cfg> [options]\n\n", prog);
    printf("Options:\n");
    printf("  --config <file>         Config file (required)\n");
    printf("  --out <file.csv>        Output CSV path (default: retirement_projection.csv)\n");
    printf("  --roi <pct>              Override annual investment return, e.g. 5.0\n");
    printf("  --inflation <pct>        Override personal spending inflation\n");
    printf("  --gov-inflation <pct>    Override CPP/OAS/tax-bracket inflation\n");
    printf("  --target-net <amount>    Override monthly household net spending target\n");
    printf("  --target-gross <amount>  Override monthly PER-PERSON gross income target\n");
    printf("  --years <n>              Override simulated horizon in years\n");
    printf("  --retire-date <YYYY-MM-DD>  Override retirement date\n");
    printf("  --tfsa <amount>          Override starting TFSA balance\n");
    printf("  --registered-a <amount>  Override person A's starting registered (RRSP/RRIF/LIF) balance\n");
    printf("  --registered-b <amount>  Override person B's starting registered (RRSP/RRIF/LIF) balance\n");
    printf("  --monte-carlo            Run Monte Carlo simulation (requires --mc-returns-file)\n");
    printf("  --mc-in-turn             Run Monte Carlo simulations in turn, starting from each year in the historical returns set once (requires mc-returns-file)\n");
    printf("\n");
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    const char *out_path = "retirement_projection.csv";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_path = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { print_usage(argv[0]); return 0; }
    }

    if (!config_path) {
        fprintf(stderr, "Error: --config <file> is required.\n\n");
        print_usage(argv[0]);
        return 1;
    }

    Config cfg;
    config_defaults(&cfg);
    if (!config_load(&cfg, config_path)) return 1;
    config_apply_overrides(&cfg, argc, argv);

    printf("Retirement simulation\n");
    printf("  %s: retirement %04d-%02d-%02d\n", cfg.a.name, cfg.retirement_date.year, cfg.retirement_date.month, cfg.retirement_date.day);
    printf("  Simulating from %04d-%02d-%02d for %d years -> %s\n\n",
           cfg.sim_start_date.year, cfg.sim_start_date.month, cfg.sim_start_date.day, cfg.sim_years, out_path);

    if (cfg.mc_run || cfg.mc_in_turn) {
        printf("Running Monte Carlo simulation with %d trials...\n", cfg.mc_trials);
	    return run_monte_carlo(&cfg, out_path) ? 0 : 1;
    }
    else {
	    return run_simulation(&cfg, out_path) ? 0 : 1;
    }
}
