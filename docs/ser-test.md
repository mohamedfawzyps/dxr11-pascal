# SER test: software reordering on the GTX 1070

`bench/sertest.cpp`, built by `build_sertest.bat` into `benchout\`. Measured 2026-09-24,
NVIDIA GeForce GTX 1070, DXR tier 1.0. See the comment at the top of the
source for what each column is.

- **plain**: one `TraceRay`; the closest-hit of the material that was hit does the shading.
- **reorder**: trace (closest-hits only record the hit, 32 bytes spilled per ray),
  counting sort by material, then shade through `CallShader`.
- **two-pass**: the same without the sort. The control.
- **layout**: `random` mixes the materials triangle by triangle, `blocked` puts
  them in large patches. **cost**: iterations of the shading loop.

Every configuration drew the same image all three ways, bit for bit, and every
sort was a permutation with nondecreasing keys.

## 32 materials, 1024 x 1024 rays, median of 15

```
layout   rays         cost |  plain ms |   reorder =   trace +   sort +   shade |  two-pass | reorder vs plain
random   coherent        8 |      5.55 |      4.27 =    2.56 +   0.46 +    1.22 |      6.17 | FASTER 1.30x
         check: 1048576 of 1048576 rays hit; reordered differs on 0, two-pass on 0; sort errors 0
random   coherent       32 |     11.10 |      4.08 =    2.41 +   0.46 +    1.21 |     14.64 | FASTER 2.72x
random   coherent      128 |     36.00 |      4.39 =    2.41 +   0.46 +    1.52 |     48.05 | FASTER 8.19x
random   coherent      512 |    137.04 |      7.75 =    2.41 +   0.46 +    4.87 |    183.69 | FASTER 17.69x
random   scattered       8 |      5.74 |      5.38 =    3.67 +   0.46 +    1.25 |      7.65 | FASTER 1.07x
         check: 1048576 of 1048576 rays hit; reordered differs on 0, two-pass on 0; sort errors 0
random   scattered      32 |     11.55 |      5.36 =    3.67 +   0.46 +    1.24 |     15.94 | FASTER 2.15x
random   scattered     128 |     36.39 |      5.68 =    3.66 +   0.46 +    1.52 |     49.50 | FASTER 6.41x
random   scattered     512 |    137.40 |      8.97 =    3.67 +   0.46 +    4.82 |    185.34 | FASTER 15.31x
blocked  coherent        8 |      1.60 |      3.18 =    1.79 +   0.63 +    0.76 |      2.26 | slower 1.99x
         check: 1048576 of 1048576 rays hit; reordered differs on 0, two-pass on 0; sort errors 0
blocked  coherent       32 |      1.86 |      3.25 =    1.79 +   0.63 +    0.83 |      2.37 | slower 1.75x
blocked  coherent      128 |      2.34 |      3.77 =    1.79 +   0.63 +    1.35 |      3.06 | slower 1.61x
blocked  coherent      512 |      5.30 |      6.97 =    1.80 +   0.63 +    4.52 |      6.26 | slower 1.32x
blocked  scattered       8 |      5.73 |      5.45 =    3.69 +   0.46 +    1.29 |      7.74 | FASTER 1.05x
         check: 1048576 of 1048576 rays hit; reordered differs on 0, two-pass on 0; sort errors 0
blocked  scattered      32 |     11.54 |      5.57 =    3.66 +   0.46 +    1.27 |     16.00 | FASTER 2.07x
blocked  scattered     128 |     36.47 |      5.85 =    3.67 +   0.46 +    1.72 |     49.59 | FASTER 6.23x
blocked  scattered     512 |    137.45 |     10.25 =    3.65 +   0.46 +    6.13 |    185.28 | FASTER 13.41x
```

## 8 materials, median of 9

```
layout   rays         cost |  plain ms |   reorder =   trace +   sort +   shade |  two-pass | reorder vs plain
random   coherent       32 |      4.09 |      3.62 =    2.01 +   0.47 +    1.13 |      4.71 | FASTER 1.13x
random   coherent      128 |     10.15 |      3.78 =    1.90 +   0.44 +    1.44 |     11.28 | FASTER 2.69x
random   scattered      32 |      4.51 |      5.29 =    3.38 +   0.44 +    1.13 |      5.90 | slower 1.17x
random   scattered     128 |     10.73 |      5.30 =    3.38 +   0.44 +    1.47 |     12.78 | FASTER 2.03x
blocked  coherent       32 |      1.72 |      3.30 =    1.78 +   0.65 +    0.87 |      2.35 | slower 1.92x
blocked  coherent      128 |      2.32 |      3.79 =    1.78 +   0.65 +    1.36 |      3.04 | slower 1.63x
blocked  scattered      32 |      4.50 |      5.31 =    3.39 +   0.44 +    1.14 |      5.89 | slower 1.18x
blocked  scattered     128 |     10.66 |      5.32 =    3.38 +   0.44 +    1.49 |     12.78 | FASTER 2.00x
```

## What it says

- Pascal's DXR runs divergent closest-hits one after another: with 32
  materials and heavy shading, plain goes from 5.3 ms (blocked) to 137 ms
  (random) for the same work.
- Reordering wins that back when shading diverges: 1.3x faster at light
  shading, 8x to 18x at heavy shading, with 32 materials.
- It loses when there is nothing to win: 1.3x to 2x slower when the
  materials are already grouped, and slightly slower with 8 materials at light
  shading.
- The sort itself is about 0.5 ms for a million rays. The cost is mostly the
  extra trace and spill.

## What it does not say

- This is the best case for reordering by design: minimal spill, equal-cost
  materials, nothing after the shading. Unreal's payload and live state are
  larger, and its raygen continues after the hit shader, which a real
  emulation has to run in a later pass.
- Unreal's own passes were not measured.
