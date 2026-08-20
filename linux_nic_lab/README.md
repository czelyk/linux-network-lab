# linux_nic_lab

A from-scratch Linux PCIe Ethernet driver, written to be **read**, not to be
installed on a real NIC. It exists to show the complete, real path from PCI
device discovery down to a byte landing in a userspace socket, using
genuine Linux kernel APIs at every step, and to explain *why* each API is
used instead of a simpler-looking alternative.

```
lspci → PCI core → edunic_probe() → BAR/DMA/rings/IRQ/NAPI → register_netdev()
   → `ip link set up` → sendto()/recvfrom() → tcpdump sees it on the wire
```

## 0. Read this first: what "edunic" actually is

**edunic is not a driver for any real, purchasable network card.** There is
no edunic silicon. The PCI Vendor/Device ID it matches
(`0xFEED:0xC0DE`, see `driver/nic.h`) is a placeholder that is not assigned
to anyone by the PCI-SIG and will never match a real device on a real
machine. This is deliberate — the project brief this was built against is
explicit that inventing a fake driver and presenting it as compatible with
real hardware would be dishonest, and that the right move when no real
target NIC exists is to make the hardware side an openly
**software-emulated educational model**.

Concretely, that means two very different things live side by side in this
codebase, and it matters to know which is which while reading it:

| | Real and load-bearing | Emulated / stands in for silicon |
|---|---|---|
| PCI subsystem calls (`pci_enable_device`, `pci_set_master`, `pci_request_regions`, `pci_iomap`, DMA mask setup) | ✅ exactly as a real driver would use them | |
| Descriptor rings, `dma_alloc_coherent`, `dma_map_single`, ownership bits, memory barriers | ✅ genuinely correct DMA API usage | |
| `net_device_ops`, NAPI, `request_irq`, `eth_type_trans`, `napi_gro_receive` | ✅ exactly as a real driver would use them | |
| The chip's internal reaction to a register write (DMA engine moving descriptors, asserting an interrupt) | | 🧪 modeled in `driver/nic_hw.c` by a kernel workqueue + hrtimer operating on the same ring memory real hardware would touch |
| The physical interrupt line / interrupt controller routing | | 🧪 there is no physical line to assert, so the emulation calls the driver's own ISR (`edunic_intr()`) directly — the *one* place this project cannot avoid diverging from real hardware, and it is called out explicitly at the top of `nic_hw.c` |

Because `edunic_probe()` only ever runs when the PCI core matches a device
against `edunic_pci_tbl`, and no such device exists anywhere, **this module
will build cleanly but will not bind to anything on a stock machine** —
which is the *correct* behavior of a real PCI subsystem faced with a driver
for hardware that isn't present, not a bug. The way to actually see it
bind and run is to pair it with a virtual PCI device that implements the
register contract documented in `driver/nic.h` (for example, a small QEMU
device model — genuinely building one is a separate, hypervisor-side
project outside this repo's scope). What you *can* do today, on this
machine, without any of that:

- Build it for real against a real kernel tree and get real compiler
  errors if you break something (`cd driver && make`).
- Read every code path — probe/remove, TX, RX, IRQ, NAPI, teardown — with
  confidence that it is what a real driver's equivalent path looks like,
  because it is written the same way a real driver's would be, and it
  compiles as one.
- Read `nic_hw.c`'s emulation half to see exactly what a NIC's internal
  DMA-engine-and-interrupt logic is doing on the *other side* of every
  register write and doorbell your driver code issues — most driver
  tutorials leave that side as a total black box.

## 1. Repository layout

```
linux_nic_lab/
├── driver/
│   ├── nic.h        shared types, register map, cross-file prototypes
│   ├── nic_main.c   module init/exit, net_device_ops, open/stop/change_mtu
│   ├── nic_pci.c    struct pci_driver, probe(), remove()
│   ├── nic_hw.c     MMIO register accessors + the emulated "chip"
│   ├── nic_tx.c     TX ring, ndo_start_xmit, TX completion reclaim
│   ├── nic_rx.c     RX ring, buffer refill, NAPI RX clean path
│   ├── nic_irq.c    ISR, NAPI poll, request_irq/free_irq
│   └── Makefile
├── userspace/
│   ├── udp_server.c / udp_client.c
│   ├── tcp_server.c / tcp_client.c
│   └── Makefile
└── README.md        this file
```

## 2. Building

```sh
cd linux_nic_lab/driver
make                      # builds edunic.ko against the running kernel
modinfo edunic.ko          # inspect it -- note the pci: alias line
```

This repository's driver was built and verified against a real, current
kernel source tree (`/lib/modules/$(uname -r)/build`) with zero warnings,
including under `make W=1`. Because it targets a very recent kernel, it
uses `hrtimer_setup()` (the combined init+callback API that replaced the
older `hrtimer_init()` + `timer.function = ...` two-step); if you're
building against an older kernel that predates that API, swap it back for
`hrtimer_init()` in `nic_hw.c`.

`modinfo` will show:

```
alias:  pci:v0000FEEDd0000C0DEsv*sd*bc*sc*i*
```

That `v0000FEED d0000C0DE` is exactly the placeholder ID from §0 — proof
the module is structurally a real, loadable PCI driver, matched against an
ID nothing on your PCI bus will ever present.

```sh
cd linux_nic_lab/userspace
make                       # builds udp_server, udp_client, tcp_server, tcp_client
```

## 3. Architecture diagrams

### 3.1 PCI discovery → probe lifecycle

```
  System boot / hotplug event
          |
          v
  +----------------------------------------------------+
  | PCI core enumerates the bus: for every function it  |
  | finds, it reads config space (Vendor ID, Device ID, |
  | Class Code, BAR sizing, ...) and builds a            |
  | struct pci_dev for it.                               |
  +----------------------------------------------------+
          |
          v
  +----------------------------------------------------+
  | For every struct pci_dev, and every struct           |
  | pci_driver ever registered via pci_register_driver(),|
  | the core compares (vendor, device) against that       |
  | driver's id_table.                                    |
  +----------------------------------------------------+
          |
          |  edunic_pci_tbl = { PCI_DEVICE(0xFEED, 0xC0DE) }
          |  matches only a device presenting exactly that ID
          v
     match found?  ------ no ------> device sits unclaimed,
          |                          visible in `lspci` with
         yes                         "Kernel driver in use:" blank
          v
  +----------------------------------------------------+
  | edunic_probe(pdev, id) is called synchronously,      |
  | once, for this device. See §3.2/§3.3 for what        |
  | happens inside.                                      |
  +----------------------------------------------------+
          |
      succeeds?  ---- no ---> staircase unwind (nic_pci.c err_* labels),
          |                   probe() returns an errno, device stays
         yes                  unbound, nothing leaked
          v
  net_device registered, visible via `ip link`,
  waiting for an administrator to bring it up
```

Module load and module unload bookend this whole picture:
`module_init()` calls `pci_register_driver()` once (which itself walks
already-enumerated devices looking for matches, i.e. the diagram above can
run *inside* that one call for a device found at boot); `module_exit()`
calls `pci_unregister_driver()`, which calls `edunic_remove()` on every
still-bound device before the driver is forgotten.

### 3.2 Control path: CPU → MMIO → NIC registers

```
   driver code, e.g. edunic_reg_write(adapter, EDUNIC_REG_TDT, i)
          |
          |  writel(val, hw_addr + reg)
          v
   __iomem virtual address  (hw_addr = value pci_iomap() returned)
          |
          |  MMU translation; this VA is mapped as strongly-ordered /
          |  device memory (non-cacheable, non-speculative), NOT as
          |  ordinary RAM
          v
   physical address = pci_resource_start(pdev, BAR) + reg
          |
          |  PCIe Memory Write Transaction-Layer-Packet, routed by the
          |  root complex and any intervening switches to the function
          |  that owns this BAR
          v
   NIC's internal register file (EDUNIC_REG_* in nic.h)
          |
          v
   chip-internal logic reacts -- e.g. a TDT write is the doorbell that
   starts the DMA engine (see §3.4); in this project that reaction is
   nic_hw.c's edunic_hw_kick_tx() queuing emulated work instead of gates
   switching, but the MMIO trip above it is identical to the real thing.
```

Why `readl()`/`writel()` and not `*(volatile u32 *)ptr`? `volatile` is a
*compiler* directive only — it stops the compiler from eliding or
reordering the access, but says nothing about CPU-level or interconnect-
level reordering, says nothing about endianness (PCIe register files are
conventionally little-endian on the wire; a raw dereference does no
swapping on a big-endian host), and produces a plain `u32 *` with none of
sparse's `__iomem` address-space checking. `readl()`/`writel()` are the
portable abstraction that gets all of that right across x86/arm64/etc. The
full version of this explanation lives as a comment directly above
`edunic_reg_read()` in `nic_hw.c`.

### 3.3 Data path: RAM ↔ DMA ↔ NIC

```
   CPU virtual address                     dma_addr_t (bus / device address)
   (skb->data, or the coherent              |
    ring pointer tx_ring->desc)             |
          |                                 |
          |   dma_map_single() / dma_alloc_coherent()
          |   translate + (for streaming maps) flush CPU caches
          +-------------------------------->|
          |                                 |
          |          +----------------------+----------------------+
          |          |   IOMMU present?                            |
          |          |     yes: dma_addr_t is an IOVA; the IOMMU    |
          |          |          translates it to the real physical |
          |          |          page(s) *on the fly*, and can deny  |
          |          |          any access outside the mapped range |
          |          |          (DMA isolation -- the device cannot |
          |          |          be tricked/exploited into reading   |
          |          |          or writing arbitrary host memory)   |
          |          |     no:  dma_addr_t degrades to physical      |
          |          |          address + offset directly            |
          |          +----------------------+----------------------+
          |                                 |
          |                                 v
          |                     PCIe memory read/write TLPs --
          |                     the device acting as bus master
          |                     (requires pci_set_master(), §4)
          |                                 |
          v                                 v
   kernel/user memory pages    <=======>    NIC DMA engine
   (physical RAM)               (device reads TX payload /
                                  writes RX payload directly,
                                  no CPU copy in the middle)
```

Two different allocators are used deliberately for two different jobs:

- **`dma_alloc_coherent()`** for the descriptor rings: both CPU and device
  poke at this memory repeatedly and unpredictably (fill a descriptor,
  check a status bit, fill the next), so it is kept coherent continuously
  by the platform and needs no explicit sync call around each touch.
- **`dma_map_single()` / `dma_unmap_single()`** (streaming DMA) for packet
  payloads: each buffer is handed to the device for exactly one transfer
  and then given back, so a cheaper one-shot mapping (with explicit
  ownership transfer) is the right tool. `DMA_TO_DEVICE` for TX buffers
  (CPU wrote it, device reads it), `DMA_FROM_DEVICE` for RX buffers
  (device writes it, CPU reads it) — see `nic_tx.c` / `nic_rx.c`.

### 3.4 TX descriptor lifecycle / ownership transitions

```
  CPU                                                      DEVICE
  ---                                                      ------
  1. edunic_start_xmit():
       dma_map_single(skb->data, DMA_TO_DEVICE)
       fill desc: buffer_addr, length, cmd=EOP|RS, status=0
       dma_wmb()                    -- descriptor writes visible
                                        before the doorbell below
       writel(TDT, next_to_use)  ------------------------------>  doorbell observed
                                                                   (edunic_hw_kick_tx
                                                                    in the emulation;
                                                                    real silicon just
                                                                    starts fetching)
                                             OWNERSHIP: CPU -> DEVICE
                                                                       |
                                                                       v
                                                          2. DMA-read the buffer_addr
                                                             the descriptor points at,
                                                             transmit it onto the wire
                                                                       |
                                                                       v
                                                          3. dma_wmb(); desc.status |= DD
                                                             writel(TDH, new_head)
                                                             raise TX-complete interrupt
                                             OWNERSHIP: DEVICE -> CPU
                       <-------------------------------------------------------------
  4. edunic_clean_tx_irq() (NAPI poll):
       sees desc.status & DD, dma_rmb()
       dma_unmap_single(..., DMA_TO_DEVICE)
       dev_consume_skb_any(skb)
       advance next_to_clean
       netif_wake_queue() if the ring had been stopped
```

### 3.5 RX descriptor lifecycle / ownership transitions

```
  CPU                                                      DEVICE
  ---                                                      ------
  1. edunic_alloc_rx_buffers():
       allocate skb, dma_map_single(DMA_FROM_DEVICE)
       fill desc: buffer_addr, length=0, status=0
       dma_wmb()
       writel(RDT, next_to_use)  ------------------------------>  buffer now owned
                                                                   by the device
                                             OWNERSHIP: CPU -> DEVICE
                                                                       |
                                                                       v
                                                          2. a frame arrives on the wire
                                                             (or, in this project, the
                                                             emulated loopback in
                                                             nic_hw.c hands one over)
                                                                       |
                                                                       v
                                                          3. DMA-write frame bytes into
                                                             the buffer at buffer_addr
                                                             set desc.length, dma_wmb(),
                                                             desc.status |= DD | EOP
                                                             writel(RDH, new_head)
                                                             raise RX interrupt
                                             OWNERSHIP: DEVICE -> CPU
                       <-------------------------------------------------------------
  4. edunic_clean_rx_irq() (NAPI poll):
       sees desc.status & DD, dma_rmb()
       dma_unmap_single(..., DMA_FROM_DEVICE)
       skb_put(skb, len)
       skb->protocol = eth_type_trans(skb, netdev)
       set skb->ip_summed from the checksum-valid status bit
       napi_gro_receive(&napi, skb)
       refill: edunic_alloc_rx_buffers() posts a fresh buffer into this slot
```

### 3.6 IRQ ↔ NAPI relationship

```
   device raises interrupt
   (real: MSI/MSI-X write, or legacy INTx line asserted;
    emulated: nic_hw.c calls edunic_intr() directly)
          |
          v
   +-----------------------------------------------------+
   | edunic_intr()  -- HARD-IRQ CONTEXT, must not sleep   |
   |   icr = readl(ICR)     "read to clear"               |
   |   if (icr & LSC)  netif_carrier_on/off()              |
   |   if (icr & (RX|TX) && napi_schedule_prep())          |
   |       writel(IMC, RX|TX)   -- mask further RX/TX ints |
   |       __napi_schedule()                               |
   +-----------------------------------------------------+
          |
          |  (deferred to softirq -- NET_RX_SOFTIRQ)
          v
   +-----------------------------------------------------+
   | edunic_poll(napi, budget)  -- SOFTIRQ CONTEXT         |
   |   edunic_clean_tx_irq()          (unbudgeted)         |
   |   work_done = edunic_clean_rx_irq(budget)              |
   |   if work_done >= budget:                              |
   |       return budget   -- NAPI calls us again, no new   |
   |                           hardware interrupt needed     |
   |                           (this is the mitigation win)  |
   |   else:                                                 |
   |       napi_complete_done(napi, work_done)                |
   |       writel(IMS, RX|TX)   -- unmask, interrupts may     |
   |                               flow again                 |
   +-----------------------------------------------------+
```

Masking RX/TX causes for the duration of a poll and only unmasking once
`edunic_poll()` reports "nothing left right now" is the standard NAPI
contract: it prevents the device from raising more interrupts for work
that is already being drained, which under load would just be pure
overhead.

### 3.7 Full UDP TX/RX path

```
 TX (sender side)                          RX (receiver side)
 ----------------                          ------------------
 sendto()                                   wire
   |                                          |
   v                                          v
 UDP layer: build UDP header,               NIC DMA engine writes frame
 checksum                                   into a posted RX buffer
   |                                          |
   v                                          v
 IP layer: build IP header,                RX descriptor marked DD,
 routing-table lookup for a                interrupt raised
 route to the destination                    |
   |                                          v
   v                                        edunic_intr() -> napi_schedule()
 neighbour subsystem: resolve                 |
 next-hop's link-layer address                v
 (ARP, cached in the neighbour              edunic_poll() -> edunic_clean_rx_irq()
 table after the first resolve)               |
   |                                          v
   v                                        skb built, eth_type_trans()
 Ethernet header prepended,                 sets skb->protocol = ETH_P_IP
 skb hits the qdisc layer                     |
   |                                          v
   v                                        IP layer: ip_rcv() -- is this
 dequeued into                              host the destination? checksum,
 ndo_start_xmit()                           routing decision (local deliver
 = edunic_start_xmit()  (nic_tx.c)          vs forward)
   |                                          |
   v                                          v
 TX descriptor filled, doorbell             UDP layer: udp_rcv() -- hash
 rung, DMA, wire (§3.4)                     lookup by (dst port[, dst addr])
                                             to find the matching socket
                                               |
                                               v
                                             datagram enqueued on that
                                             socket's receive queue
                                               |
                                               v
                                             recvfrom() in udp_server /
                                             udp_client returns
```

### 3.8 Full TCP TX/RX path

TCP rides the identical driver TX/RX entry points as UDP — the same
`edunic_start_xmit()` and the same `edunic_clean_rx_irq()` handle every
TCP segment. What TCP adds lives entirely in the kernel's TCP state
machine, above the driver and above IP:

```
 connect() [client]                          listen()+accept() [server]
     |                                              |
     |  1. kernel builds a SYN segment              | socket in LISTEN state,
     |     (initial sequence number chosen)         | SYN queue ready
     |     --> IP --> driver TX (§3.4) --> wire      |
     |                                              v
     |                                        SYN arrives via driver RX
     |                                        (§3.5) -> IP -> TCP: a
     |                                        half-open request is queued
     |                                              |
     |                                              v
     |                              2. kernel replies SYN-ACK (its own
     |                                 ISN, ACK = client ISN + 1)
     |                                 --> driver TX --> wire
     v
 SYN-ACK arrives via driver RX (§3.5) -> IP -> TCP:
 client's connect() is about to return
     |
     |  3. kernel sends the final ACK
     |     --> driver TX --> wire
     v                                              |
 connect() returns: ESTABLISHED           ACK arrives via driver RX -----> accept() returns
                                           connection moves from the SYN     a connected socket:
                                           queue to the accept queue         ESTABLISHED

 --- data phase (either direction, symmetric) ---
 write()/send() -> TCP segments each carry a sequence number covering
 the bytes they contain -> IP -> driver TX -> wire
                                                    |
                                                    v
                                    driver RX -> IP -> TCP: segment's
                                    sequence number checked against the
                                    expected next byte; if in order, data
                                    is queued to the socket's receive
                                    buffer and an ACK (piggybacked or
                                    standalone) is scheduled
                                                    |
                                                    v
                                    ACK --> driver TX --> wire --> sender's
                                    driver RX --> TCP: matches the segment
                                    against its retransmit queue, cancels
                                    the retransmit timer for those bytes

 If no ACK arrives before the retransmit timer (RTO) expires, TCP
 re-sends the unacked segment -- through the exact same
 edunic_start_xmit() path as the original, indistinguishable to the
 driver from any other outbound segment. Retransmission, congestion
 control, and reassembly are all TCP-layer concepts with zero visibility
 into or special-casing by the driver.

 --- teardown ---
 close() -> FIN segment -> driver TX -> wire -> peer's driver RX -> TCP
 sees FIN, ACKs it, and (once the peer also closes) the connection walks
 through FIN_WAIT/TIME_WAIT -- again, ordinary segments through the
 ordinary TX/RX paths.
```

### 3.9 Ownership-transition summary

The single idea underlying §3.4–§3.5: every descriptor is, at every
instant, owned by exactly one side, and a `dma_wmb()`/`dma_rmb()` pair
around the status-bit write/read is what makes that ownership change
visible and safe to act on across the CPU/device boundary. Skipping the
barrier would let one side observe a descriptor as "done" while its other
fields (length, buffer address) are still mid-write from the other side's
point of view on a weakly-ordered architecture.

```
   TX descriptor:  [ CPU fills fields ] -> DOORBELL -> [ DEVICE owns ] -> DD set -> [ CPU owns again ]
   RX descriptor:  [ CPU posts empty buffer ] -> DOORBELL -> [ DEVICE owns ] -> DD set -> [ CPU owns again ]
```

## 4. Register map (placeholder — see `driver/nic.h`)

All offsets, bit numbers, and the overall shape (per-ring base/length/head/
tail registers, a read-to-clear ICR + separate IMS/IMC mask registers) are
**invented for this project**, loosely modeled on the *style* of real
legacy-descriptor NICs because that style is genuinely representative —
but they match no real datasheet. Every access goes through
`edunic_reg_read()`/`edunic_reg_write()` in `nic_hw.c`, never a raw
pointer. Full field-by-field documentation is in the comment block above
the `#define`s in `nic.h`.

## 5. The software-emulated NIC model

`nic_hw.c` is split into two clearly-separated halves — see the file
header comment there for the details already summarized in §0's table.
The short version: a `struct work_struct` reacts to the TX doorbell by
walking newly-posted TX descriptors, "transmitting" each one (looping its
bytes back onto the RX ring, so a UDP/TCP echo test genuinely round-trips
through both TX and RX), marking completion, and raising an interrupt; an
`hrtimer` fires once after `edunic_hw_start()` to model PHY autonegotiation
delay before link comes up. Both act only on memory a real DMA engine
would legitimately touch (the coherent rings, and streaming buffers via
proper `dma_sync_single_for_*()` calls), and both signal the driver only
through the same register writes + `edunic_intr()` call a real interrupt
controller invoking a real ISR would produce.

## 6. Where the big concepts live in this codebase

| Concept | Where |
|---|---|
| `struct pci_dev` | Represents one PCI function on the bus; the kernel creates it during enumeration and hands it to `edunic_probe(struct pci_dev *pdev, ...)` in `nic_pci.c`. Everything PCI-config-space-related (BARs, enable/disable, DMA mask) hangs off it. |
| `struct device` | The generic driver-model base every bus-specific device type (including `pci_dev`, via `pdev->dev`) embeds. `dma_alloc_coherent()`/`dma_map_single()` take a `struct device *` because DMA addressing is a property of *the bus a device sits on*, not of PCI specifically — `&adapter->pdev->dev` is passed everywhere in `nic_tx.c`/`nic_rx.c`. |
| `struct net_device` | The kernel's representation of a network interface (`eth0`-style). Allocated by `alloc_etherdev()` in `nic_pci.c`, published by `register_netdev()`; `netdev_priv()` recovers our `struct edunic_adapter` from it. |
| `struct sk_buff` | The universal packet buffer. Built by the stack for TX (handed to us in `edunic_start_xmit()`), built by us for RX (`netdev_alloc_skb_ip_align()` in `nic_rx.c`, then handed to the stack via `napi_gro_receive()`). |
| BAR (Base Address Register) | PCI config-space register describing one address range the device exposes. BAR0 here is the MMIO register window; sized via `pci_request_regions()`, mapped via `pci_iomap()` in `nic_pci.c`. |
| MMIO | §3.2. Registers, not memory — accessed via `readl()`/`writel()`, never treated as cacheable RAM. |
| DMA | §3.3. Two flavors used: coherent (rings) and streaming (payloads). |
| Descriptor rings | §3.4/§3.5, `nic_tx.c`/`nic_rx.c`. Fixed-size circular arrays of hardware descriptors + a parallel software metadata array, indexed identically. |
| IRQ | §3.6, `nic_irq.c`. |
| MSI/MSI-X | `edunic_setup_interrupts()` in `nic_irq.c` requests one vector via `pci_alloc_irq_vectors(..., PCI_IRQ_MSI | PCI_IRQ_INTX)`, preferring MSI (a normal DMA memory write that carries the interrupt, avoiding legacy INTx's shared-line ambiguity and needing no separate ack cycle) and falling back to INTx. A real multi-queue NIC would request one MSI-X vector *per queue* (`PCI_IRQ_MSIX`) so each queue's NAPI can be steered to and processed on a different CPU; this driver has one queue, hence one vector. |
| NAPI | §3.6, `nic_irq.c`. |
| MTU / Jumbo Frames | `netdev->min_mtu`/`max_mtu` set in `nic_pci.c`; `edunic_change_mtu()` in `nic_main.c`. This driver deliberately caps at the standard 1500-byte MTU because its RX buffers are a single fixed-size, non-chained allocation (`EDUNIC_RX_BUF_SIZE` in `nic.h`) — true jumbo-frame support needs either bigger buffers or RX scatter-gather (next row), which is explained but not implemented, to keep the ring bookkeeping in `nic_rx.c` focused on the ownership/DMA lesson rather than multi-descriptor reassembly. |
| Checksum offload | RX side only, honestly: `EDUNIC_RXD_STAT_CSUM_VALID` in a descriptor's status maps to `skb->ip_summed = CHECKSUM_UNNECESSARY` in `edunic_clean_rx_irq()` (`nic_rx.c`); otherwise `CHECKSUM_NONE`. No TX checksum offload is implemented — `desc->cso`/`css` fields exist in the hardware descriptor as documented placeholders for where it would hook in, but `edunic_start_xmit()` never sets them, so the stack computes TX checksums in software as it would for any device that doesn't advertise `NETIF_F_HW_CSUM`. |
| Scatter-gather | Not implemented — every skb here is transmitted/received as one physically-contiguous linear buffer, one descriptor per packet. A scatter-gather-capable driver would walk `skb_shinfo(skb)->frags[]` and fill one descriptor per fragment, letting the stack avoid ever linearizing a paged skb before handing it to the driver (`NETIF_F_SG`). |
| GSO/TSO/GRO | GRO is implemented (`napi_gro_receive()` in `nic_rx.c`) — it's a *software* stack facility, not a hardware capability, so it needs no driver-side offload flag. TSO (hardware segmentation of a huge TCP send into MTU-sized frames, advertised via `NETIF_F_TSO`) is not implemented; without it, GSO still happens, just in software (`skb_gso_segment()`) before segments individually reach `edunic_start_xmit()`. |
| XDP | Not implemented. Would hook in as an early-as-possible check inside `edunic_clean_rx_irq()`, before the `eth_type_trans()`/skb-allocation path, running an eBPF program directly against the raw DMA buffer to allow drop/redirect/pass decisions before the cost of building an skb is paid at all. |
| AF_PACKET | Not part of the driver — a userspace program can already open an `AF_PACKET` socket on the `edunic` interface (once bound to real/virtual hardware) to see raw Ethernet frames the same way `tcpdump` does, entirely via the generic `netif_receive_skb()`/packet-taps path every net_device gets for free once it hands frames up via `napi_gro_receive()`. |
| `PACKET_MMAP` | An `AF_PACKET` extension (`setsockopt(PACKET_RX_RING/TX_RING)`) that maps a ring buffer directly into userspace to avoid a copy per packet — again generic kernel infrastructure this driver participates in automatically by being a normal net_device, no driver-side code needed. |
| tcpdump / libpcap / Wireshark | See §7 — these attach via `AF_PACKET` taps on the `edunic` interface, upstream of routing/socket delivery, so they can observe both TX and RX traffic this driver moves. |
| Netfilter | Hooks (`NF_INET_PRE_ROUTING`, `NF_INET_LOCAL_IN`, etc.) run in the IP layer, between "skb delivered by the driver via `napi_gro_receive()`" and "socket receive queue" on RX, and between "socket send" and "driver TX" on the way out — entirely above the driver, invisible to it, but sitting directly in the UDP/TCP RX/TX paths drawn in §3.7/§3.8. |

## 7. Userspace test environment

Build the tools:

```sh
cd linux_nic_lab/userspace && make
```

If you have a matching virtual PCI device bound (§0) so the interface is
real and up, here's the full toolkit for observing it, in the order you'd
typically reach for them:

```sh
# 1. Confirm the PCI device and driver binding
lspci -nn | grep -i "feed:c0de\|Ethernet"
lspci -vvv -d feed:c0de           # full config space, BAR sizes, MSI capability

# 2. See it as a network interface, assign an address, bring it up
ip link show edunic0                # or whatever udev names it
ip addr add 192.168.77.1/24 dev edunic0
ip link set edunic0 up
ip -s link show edunic0             # TX/RX packet & byte counters, drops, errors --
                                     # ties directly to struct edunic_stats

# 3. Driver / hardware-facing details
ethtool edunic0                     # link state (from netif_carrier_on/off, §3.4 emulated PHY)
ethtool -i edunic0                  # driver name/version -- from edunic_get_drvinfo()
cat /proc/interrupts | grep edunic  # confirm the IRQ vector is registered and firing
                                     # (compare MSI vs legacy INTx line usage, §6)

# 4. Watch traffic in flight
tcpdump -i edunic0 -n -v            # every frame the driver TX's or RX's, at the
                                     # AF_PACKET tap point mentioned in §6

# 5. Generate traffic
./udp_server 5000 &
./udp_client 192.168.77.1 5000 "hello"

./tcp_server 5001 &
./tcp_client 192.168.77.1 5001 "hello"

# optional: sustained throughput test
iperf3 -s -B 192.168.77.1 &
iperf3 -c 192.168.77.1
```

Run `udp_server`/`tcp_server` and the matching client in separate
terminals (or backgrounded, as above) and watch `tcpdump` in a third —
you'll see each request go out and the echo come back, each one a full
trip through §3.7 or §3.8.

## 8. Guided walkthrough — reading the source in runtime order

This mirrors exactly how the system actually executes, start to finish:
module load → PCI match → probe → PCI resources → MMIO → DMA → rings →
IRQ/NAPI → `register_netdev` → interface up → UDP/TCP TX → completion →
RX → socket → interface down → remove.

1. **`nic_main.c: edunic_init_module()`** — the very first code that runs.
   Calls `pci_register_driver(&edunic_pci_driver)`.
2. **`nic_pci.c: edunic_pci_tbl[]` / `edunic_pci_driver`** — the data the
   PCI core matches against, and the probe/remove function pointers it
   will call. (§3.1)
3. **`nic_pci.c: edunic_probe()`** — read this one top to bottom; it's
   written as a linear acquire-in-order / unwind-in-reverse-order
   sequence: `pci_enable_device()` → `pci_set_master()` →
   `pci_request_regions()` → `pci_iomap()` (§3.2) → DMA mask → `alloc_etherdev()`
   → `edunic_hw_reset()` → MAC address setup → `edunic_setup_tx_resources()`
   / `edunic_setup_rx_resources()` (§3.3) → `edunic_hw_init_rings()` →
   `edunic_alloc_rx_buffers()` → `edunic_hw_emulation_init()` →
   `edunic_napi_add()` → `register_netdev()`.
4. **`nic_hw.c: edunic_reg_read/write(), edunic_hw_reset/init_rings/start/stop()`**
   — what probe() (and later `edunic_open()`/`edunic_stop()`) actually calls
   into for every register touch. Read the file header comment for the
   MMIO-vs-`volatile` explanation (§3.2) before the accessors themselves.
5. **`nic_tx.c: edunic_setup_tx_resources()`, `nic_rx.c: edunic_setup_rx_resources()`**
   — `dma_alloc_coherent()` for the rings, `kcalloc()` for the parallel
   software metadata arrays. This is where §3.3's coherent-vs-streaming
   distinction first becomes concrete.
6. **`nic_irq.c`** — read `edunic_intr()` and `edunic_poll()` together
   with the file header's IRQ↔NAPI sequence (§3.6) before their
   registration helpers (`edunic_setup_interrupts()`, `edunic_napi_add()`).
7. **Back to `nic_main.c: edunic_open()`** — the interface is now
   administratively up (`ip link set up`). Enables NAPI, requests the IRQ,
   then `edunic_hw_start()` — this exact ordering (listener ready before
   the device is told it may raise interrupts) is worth internalizing.
8. **TX: `nic_tx.c: edunic_start_xmit()`** — trace one packet through
   `dma_map_single()` → descriptor fill → `dma_wmb()` → doorbell write →
   `edunic_hw_kick_tx()`. Compare against §3.4.
9. **TX completion: `nic_hw.c: edunic_hw_tx_worker()`** — the "device
   side" reaction to the doorbell you just traced, then back to
   **`nic_tx.c: edunic_clean_tx_irq()`** — the driver reclaiming what the
   worker marked done.
10. **RX delivery: `nic_hw.c: edunic_hw_deliver_rx_frame()`** then
    **`nic_rx.c: edunic_clean_rx_irq()`** — trace the same packet's bytes
    landing in a fresh skb, `eth_type_trans()`, checksum metadata, and
    `napi_gro_receive()`. Compare against §3.5.
11. **Userspace: `userspace/udp_client.c` / `udp_server.c` (or the tcp_*
    pair)** — read the file header comments there for exactly where
    `sendto()`/`recvfrom()` (or `connect()`/`accept()`) sit relative to
    everything above, i.e. §3.7/§3.8 made concrete.
12. **`nic_main.c: edunic_stop()`** — interface brought down: stop the
    queue, `edunic_hw_stop()`, `edunic_free_interrupts()`,
    `edunic_napi_disable()`. Note this is the mirror image of step 7, in
    reverse.
13. **`nic_pci.c: edunic_remove()`** — module unload / unplug. Mirrors
    `edunic_probe()`'s acquisition order in exact reverse:
    `unregister_netdev()` (which itself invokes step 12 if the interface
    was still up) → `edunic_napi_del()` → `edunic_hw_emulation_uninit()` →
    free RX/TX resources → `pci_iounmap()` → `pci_release_regions()` →
    `free_netdev()` → `pci_disable_device()`.
14. **`nic_main.c: edunic_exit_module()`** — `pci_unregister_driver()`,
    which calls step 13 for every still-bound device before deregistering.

If you only read six functions in this whole repository, make them:
`edunic_probe()`, `edunic_start_xmit()`, `edunic_hw_tx_worker()`,
`edunic_clean_rx_irq()`, `edunic_intr()`/`edunic_poll()`, and
`edunic_remove()` — in that order, they *are* the driver.
