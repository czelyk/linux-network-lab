#include <stdio.h>
#include <stdlib.h>
#include <pcap.h>

int main(void)
{
    pcap_t *handle;
    char errbuf[PCAP_ERRBUF_SIZE];

    handle = pcap_open_live(
        "wlo1",
        65535,
        1,
        1000,
        errbuf
    );

    if(handle == NULL)
    {
        fprintf(stderr, "pcap_open_live: %s\n", errbuf);
        return EXIT_FAILURE;
    }

    printf("Capture started on wlo1\n");

    pcap_close(handle);

    return EXIT_SUCCESS;
}