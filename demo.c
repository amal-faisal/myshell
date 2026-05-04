#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    //parsing the burst time from argument
    if (argc < 2)
    {
        fprintf(stderr, "usage: demo <seconds>\n");
        return 1;
    }

    int seconds = atoi(argv[1]);
    if (seconds <= 0)
    {
        fprintf(stderr, "Error: seconds must be positive\n");
        return 1;
    }

    //looping for specified number of seconds
    for (int i = 1; i <= seconds; i++)
    {
        //outputting progress line
        printf("[DEMO] Running... iteration %d/%d\n", i, seconds);
        fflush(stdout);

        //sleeping 1 second
        sleep(1);
    }

    printf("[DEMO] Complete after %d seconds\n", seconds);

    return 0;
}
