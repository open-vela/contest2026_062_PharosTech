# VEPU510 H.264 encoder

The driver encodes one picture at a time and predicts each picture from the
reconstruction of the picture before it.  What that is worth, and how long a
group of pictures it is worth doing it over, was measured rather than assumed;
this file records the measurement.

## Group length

Two pictures per group is the default
(`RK3576_VEPU_DEFAULT_GOP`), and `camenc` follows it
(`CAMENC_DEFAULT_GOP`).  The numbers below are the reason.

Measured on a 640x480 capture of a static scene -- a ceiling light, no motion
in frame, 60 frames, quantiser 26, no rate control -- from the driver's own
streams:

| pictures per group | IDR | P | payload | rate | against all-intra |
| --- | --- | --- | --- | --- | --- |
| 1 (all intra) | 60 | 0 | 186680 B | 1454 kbit/s | -- |
| 2 | 30 | 30 | 95862 B | 747 kbit/s | **-48.7%** |
| 4 | 15 | 45 | 90487 B | 705 kbit/s | -51.5% |

The all-intra row is not a capture.  It is the IDR mean of the other two
multiplied by 60, which is sound because the IDR mean is the same at both
group lengths to within 0.03% (3111 B against 3110 B) -- the cost of an IDR
does not depend on what surrounds it.

Two things fall out of the table.

**Two is where the saving is.**  Going from one picture per group to two takes
48.7% off the rate, and it all comes from the P pictures: a P is 84 bytes
where an IDR is 3111, which is 2.7%.

**Four is not.**  Doubling the group again halves the number of IDRs and buys
5.6%.  The reason is visible in the per-frame sizes.  At four pictures per
group the sizes settle into a repeating shape:

```
I 3010   p 100   p 3384   p  14
I 3006   p  12   p 3380   p  12
I 3014   p   9   p 3370   p  16
```

One P in every group costs as much as the IDR that opened it -- 3380 bytes
against 3010.  Those frames are worth 42415 of the 47830 bytes that doubling
the group saves.  Had they been as cheap as the P pictures at two per group,
four per group would have come out at 374 kbit/s; instead it comes out at 705.

Nothing is wrong with the streams.  Decoding both captures and comparing them
frame for frame gives an RMSE of about 1.1 out of 255, for every frame after
the first two, so the two encodings are the same pictures.  And within a
group at four, the reconstruction never gets more than 1.2 RMSE away from the
IDR that opened it.  This is a rate-efficiency finding, not a correctness one.

**The expensive P is not explained.**  It is reproducible on this scene and
appears in no other capture we have.  The other one, `out.mp4`, is also four
pictures per group at 640x480 and its P pictures are 9 bytes throughout; its
IDRs average 920 bytes against this scene's 3110, so it is the simpler picture
of the two, and that is a correlation rather than an explanation.  The frame is
not tied to the driver's reference handling either: the driver uses two
reconstruction slots and a job writes the one it is not reading, so every P
reads the picture immediately before it, at every group length.  Raising the
slot count to four was tried and produced byte-identical streams, which rules
the slots out.  A note on `rk3576_vepu_selftest_sequence()`, in
`rk3576_vepu.c`, describes a synthetic source on which the same thing happens
far more severely; that source is pathological -- a hard vertical edge walking
across the frame -- and is close to the worst input a predictive encoder can be
given.  It is kept as a canary, not as evidence about this scene.

So the default is set where the measurement is solid and no un-explained frame
is involved.  A caller that wants 5.6% and accepts that some content gives it
away passes a longer group to `RK3576_VEPU_CID_GOP`.

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

None of this is a workaround; it is why the driver sets its own default rather
than inheriting MPP's, and why the default is short.

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
