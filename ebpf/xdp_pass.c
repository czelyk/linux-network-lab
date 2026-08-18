#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

SEC("xdp")
int xdp_pass_all(struct xdp_md *ctx)
{
    void *data;
    void *data_end;

    data = (void *)(long)ctx->data;
    data_end = (void *)(long)ctx->data_end;

    if(data >= data_end)
        return XDP_ABORTED;

    return XDP_PASS;
}

char LICENSE[] SEC("license") = "GPL";