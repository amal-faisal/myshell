#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
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

    for (int i = 0; i <= seconds; i++)
    {
        printf("Demo %d/%d\n", i, seconds);
        fflush(stdout);

        if (i < seconds)
        {
            sleep(1);
        }
    }

    return 0;
}
