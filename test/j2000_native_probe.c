/* Diagnostic only: evaluate the unprojected series, before sf_frame_xyz,
 * nutation, aberration, or any event solver.  Including the implementation
 * lets this tool access static evaluators without adding a public API. */
#include <stdio.h>
#include "../core/sf_series.c"

int main(void)
{
    double jd;
    while (scanf("%lf", &jd) == 1) {
        double earth[3], earth_default[3], moon[3];
        sf_earth_values(jd, SF_EARTH_L_BUD_FULL, SF_EARTH_B_BUD_FULL,
                        SF_EARTH_R_BUD_FULL, earth);
        sf_earth_values(jd, SF_DEF_L_BUD, SF_DEF_B_BUD,
                        SF_DEF_R_BUD, earth_default);
        sf_moon_values(jd, SF_MOON_L_BUD_FULL, SF_MOON_B_BUD_FULL,
                       SF_MOON_R_BUD_FULL, moon);
        /* Moon L/B are ELP-native; the Python checker applies the theory's
         * built-in P/Q conversion before comparing with fixed J2000. */
        printf("%.10f %.17g %.17g %.17g %.17g %.17g %.17g %.17g\n",
               jd, earth[0], earth[1], earth[2], earth_default[2],
               moon[0], moon[1], moon[2]);
    }
    return ferror(stdin) || ferror(stdout) ? 1 : 0;
}
