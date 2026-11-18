
#include "classifier.h"

/*
 * Process classification using rolling averages.
 * These are project-defined heuristic thresholds.
 */

ProcessClass classify_process(ProcessInfo* p)
{
    double average_cpu = 0.0;
    double average_io = 0.0;
    unsigned int i;
    unsigned int n;

    if (p == NULL)
        return CLASS_WARMING;

    /* Require at least three valid samples */
    if (p->samples < 3)
        return CLASS_WARMING;

    n = p->hist_count;

    if (n == 0)
        return CLASS_WARMING;

    /* Calculate rolling averages */
    for (i = 0; i < n; i++)
    {
        average_cpu += p->cpu_hist[i];
        average_io += p->io_hist[i];
    }

    average_cpu /= n;
    average_io /= n;

    /* Rule 1: CPU-bound */
    if (average_cpu >= 3.0)
        return CLASS_CPU;

    /* Rule 2: I/O-bound */
    if (average_io >= 256.0 && average_cpu <= 10.0)
        return CLASS_IO;

    /* Rule 3: Idle */
    if (average_cpu <= 0.5 && average_io <= 32.0)
        return CLASS_IDLE;

    /* Rule 4: Balanced */
    return CLASS_BALANCED;
}
