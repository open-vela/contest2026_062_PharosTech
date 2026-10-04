# VEPU510 H.264 encoder

The driver encodes one picture at a time and predicts each picture from the
reconstruction of the picture before it.  What that is worth, and how long a
group of pictures it is worth doing it over, was measured rather than assumed;
this file records the measurement.

## Group length

A group is one IDR followed by pictures that predict from it and from each
other, so its length is what decides how much of a stream is the expensive
kind.  Two questions get two answers here: the driver's default is two
pictures (`RK3576_VEPU_DEFAULT_GOP`) and the streaming application asks for
fifteen (`CAMENC_DEFAULT_GOP`).  The driver does not know how its stream is
consumed, so it takes the shortest group that predicts at all; the
application does know, so it takes the group that is worth having and pays for
it knowingly.

### Measured after the fault was fixed

This is the number to use.  1296x960 at quantiser 26, thirty frames a second,
on a source the encoder is asked to draw itself -- one constant background
with a small square moving slowly round it -- over 597 pictures.  Payload
bytes, so the container is not counted:

| pictures per group | IDR | P | payload | rate |
| --- | --- | --- | --- | --- |
| 2 | 299 at 31350 B | 298 at 253 B | 9449442 B | 3799 kbit/s |
| 4 | 150 at 31368 B | 447 at 248 B | 4816531 B | **1937 kbit/s** |

Doubling the group halves the rate -- four is 51.0% of two -- and where the
bytes are says why.  Half the groups at two open with an IDR; a quarter of
them at four do.  Nothing else stands out, because a P is under one per cent
of an IDR and no P differs much from its neighbours:

| position in the group | 0 | 1 | 2 | 3 |
| --- | --- | --- | --- | --- |
| mean bytes | 31368 | 256 | 238 | 250 |

The quality is not what is being spent.  Decoding both streams and comparing
every picture with the source gives an RMSE of 0.78, and it is 0.78 for the
IDR and for each of the three P positions of a four-picture group: nothing
accumulates through the group.

Two numbers are enough to say what a longer group would do, because the rate
is `fps * 8 * (IDR/G + P)`: 3824 and 1942 kbit/s for two and four against the
3799 and 1937 measured, and 561 for fifteen.  What that also says is where
the model stops being the answer.  The only term that falls with the group is
the IDR's, and it falls as 1/G, so the rate keeps improving -- but the price
of a long group is not in the rate.  A decoder that joined late, or that
missed a fragment, has to wait for the next picture a stream may start on,
which is one group away, and a fragment missed is not one picture lost but
everything up to that next start.

Fifteen is where those meet for a stream watched live: half a second at
thirty frames a second, which is a hiccup and not a wait, and far enough along
the curve that the rate is already a thirteenth of all-intra's.  Doubling it
again would take another 251 kbit/s off and add another half second to every
client's wait -- which is the trade, stated in the numbers it is made in.

### Measured before it was fixed, which is what the fault looked like

The same measurement was taken on a 640x480 capture of a static scene -- a
ceiling light, no motion in frame, 60 frames, quantiser 26, no rate control --
while the encoder carried the fault described below.  It said a long group was
not worth having:

| pictures per group | IDR | P | payload | rate | against all-intra |
| --- | --- | --- | --- | --- | --- |
| 1 (all intra) | 60 | 0 | 186680 B | 1454 kbit/s | -- |
| 2 | 30 | 30 | 95862 B | 747 kbit/s | **-48.7%** |
| 4 | 15 | 45 | 90487 B | 705 kbit/s | -51.5% |

The all-intra row is not a capture.  It is the IDR mean of the other two
multiplied by 60, which is sound because the IDR mean is the same at both
group lengths to within 0.03% (3111 B against 3110 B) -- the cost of an IDR
does not depend on what surrounds it.

**Two was where the saving was.**  Going from one picture per group to two
took 48.7% off the rate, and it all came from the P pictures: a P was 84 bytes
where an IDR was 3111, which is 2.7%.

**Four was not.**  Doubling the group again halved the number of IDRs and
bought 5.6%.  The per-frame sizes said why:

```
I 3010   p 100   p 3384   p  14
I 3006   p  12   p 3380   p  12
I 3014   p   9   p 3370   p  16
```

One P in every group cost as much as the IDR that opened it -- 3380 bytes
against 3010 -- and those frames were worth 42415 of the 47830 bytes that
doubling the group saved.  Had they been as cheap as the P pictures at two per
group, four per group would have come out at 374 kbit/s; instead it came out
at 705.

Nothing was wrong with the streams, which is what made this look like a
property of the content: decoding both captures and comparing them frame for
frame gave an RMSE of about 1.1 out of 255 for every frame after the first
two, and within a group the reconstruction never got more than 1.2 RMSE away
from the IDR that opened it.

**The expensive P was the fault.**  It is explained now.  The register layer
programmed the anti-smear stage from the previous picture's state, which is
what the newer vendor code does and what the 1.0.6 library this driver was
measured against does not; with that state in force, a picture predicted from
a reference with its own local mean taken out of it.  The residual that leaves
is wrong, so the rate-distortion decision finds prediction not worth taking
and codes those pictures almost as intra -- which is exactly a P that costs
what an IDR costs, on content whose P pictures are otherwise nearly free.
Writing the 1.0.6 constants instead made both symptoms go: the pictures, which
had been drifting by the reference's own mean, and the rate.  The change is in
the driver's history.  A note on `rk3576_vepu_selftest_sequence()`, in
`rk3576_vepu.c`, describes a synthetic source on which the same thing used to
happen far more severely; that source is pathological -- a hard vertical edge
walking across the frame -- and is close to the worst input a predictive
encoder can be given.  It is kept as a canary, not as evidence about a scene.

Both tables are kept because they are the two halves of one lesson: a fault in
the prediction of P pictures shows up as a rate as much as as a picture, and
the rate finding was read as a fact about the content for as long as the
picture finding was read as a separate bug.

## Upstream

Rockchip's own answer, in `rockchip-linux/mpp` issue #211 ("1080@30fps in
2 Mbps encoding has ghosting"), was that the encoder has a strong preference
for coding skip pictures and that the resulting artefacts are not something MPP
is responsible for -- the reporter was told to raise it on their redmine
instead, with their FAE.  The same "a skip picture cannot be a reference"
mechanism is visible in MPP's source: it carries a `force_pskip_is_ref` option
in both its H.264 and its H.265 encoder, and its H.265 decoder picture buffer
has a comment about holding a reference buffer as a skip picture's
reconstruction buffer.  That option is off by default.  Issue #961, on this
same silicon with a long group, is open and unresolved.

None of this is a workaround, and none of it is what this driver had: the
fault above was in the state the driver programmed, not in the silicon's
handling of skip pictures.  It is kept here because a long group is what makes
the question worth asking -- issue #961 is a report of horizontal bars from
this encoder, on this silicon, with a long group, and it is unresolved.

## The self-test

`rk3576_vepu_selftest()` runs at boot when `CONFIG_RK3576_VEPU_SELFTEST` is
set, and is the reason several of the above claims are checkable rather than
asserted.  It has two halves, and it runs the sequence test first because what
that test observes is the state before any picture has been encoded -- no
reference to predict from.

  * The **sequence test** encodes a moving source as an IDR followed by P
    pictures and checks that a P comes out cheaper than its own reference.
    Failing that check means prediction has stopped working, which is the
    failure that is otherwise silent: the stream still decodes, it is just at
    the wrong rate.
  * The **picture test** encodes static, gradient and noise sources and checks
    that the sizes come out in the order the sources' complexity implies, and
    that a NAL ends where the encoder said it did.

`CONFIG_RK3576_VEPU_SEQ_DUMP` adds a hex dump of the coded stream, which is
not on by default because it is for reading with a decoder rather than from a
terminal.

## What it prints at boot

Five lines, and nothing else:

```
VEPU0: aclk=... hclk=... core=... (targets ... and ...)
VEPU0: VEPU510 rev ..., ip_id 0x..., h264=... hevc=... bframe=... fbc=..., res=..., osd=..., filter=...
VEPU0 sequence test: N frames, N bytes: IDR N B mean, P N B mean (N% of an IDR)
VEPU0 self-test: N frames, N bytes of bitstream, solid L/s, gradient L/s, noise L/s
VEPU0 codec self-test: N frames through the device node, N bytes of bitstream
```

They report the two facts worth having at every boot -- which IP answered,
and whether the encode path works -- once for each layer that can break
independently: the hardware, the encode path, and the device node an
application actually talks to.  The clock rates are there because a core
clock left on its reset source makes the encoder work at a fraction of its
speed and say nothing about it.

The sequence test's line is the one to read if a stream is larger than it
should be: it reports the mean P picture against the mean IDR, and a P that
is not a small fraction of an IDR means prediction has stopped being used,
which is a failure that decodes correctly and only shows as size.

Everything else the driver knows -- which clock source each branch settled
on, what the dividers hold, the size of the reconstruction sets, a
frame-by-frame account of both self-tests -- goes through `vinfo()`, which
NuttX enables with `CONFIG_DEBUG_VIDEO_INFO` (its video subsystem debug
switch, off by default).  The detail is kept rather than deleted, so that it
can be asked for; it just is not printed twenty-odd lines at a time in front
of whatever does deserve attention.

Errors are not affected: `_err` is reported whatever the debug options say,
and `rk3576_vepu_dump()` prints the registers when an encode fails.

