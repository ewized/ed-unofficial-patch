# Per-permutation measurements

## Status

Complete: 32 supported requests sampled, with three clean 30-second CPU/GPU windows each (96 windows). Eight 3x requests are unsupported and were never applied.

Measured code: `cfe5e8ca4241b7169eea524f6d50fc4c5f64c620`. All rows use the parked Pilot Assessment scene and the automated virtual HMD described in the [settings and protocol](README.md). Physical Beyond inventory is documented separately; physical scanout and physical gaze are unqualified.

## Rates and timings

Rates are successful app XR submission counter deltas per second. Native, generated and repeated classifications are separate; total includes all three. They do not measure physical display FPS. Timing triples are p50/p95/p99 in milliseconds, aggregated as medians of accepted per-window percentiles. CPU/GPU and runtime cycles have separate sample populations. RealPresent is the mean of the actual DXGI Present phase. See [definitions and coverage](README.md) and [all raw fields](rows.csv).

Inactive means the requested feature did not produce its required active output. These rows document fallback behavior; they do not establish active-feature performance. Unknown values stay unavailable. In native AA-off FG-on rows, repeats can keep total submission rate near baseline while native rate falls and generated delivery remains zero.

| Mode | VRS | FG | Native/s | Generated/s | Repeated/s | Total/s | CPU p50/95/99 ms | GPU p50/95/99 ms | Cycle p50/95/99 ms | RealPresent mean ms | Qualification |
| --- | --- | --- | ---: | ---: | ---: | ---: | --- | --- | --- | ---: | --- |
| Native | off | off | 32.622 | 0.000 | 0.000 | 32.622 | 6.433/7.996/8.783 | 12.080/13.153/13.705 | 30.633/31.462/31.793 | 0.072 | measured |
| Native | off | auto | 16.987 | 0.000 | 15.641 | 32.628 | 7.097/8.513/9.406 | 27.258/28.495/28.978 | 61.261/62.993/63.893 | 0.076 | inactive |
| Native | off | 2x | 16.314 | 0.000 | 16.314 | 32.629 | 7.043/8.392/9.839 | 27.330/28.630/29.039 | 61.309/62.918/63.683 | 0.077 | inactive |
| Native | off | 3x | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unsupported |
| Native | off | 4x | 8.161 | 0.000 | 24.484 | 32.645 | 7.547/8.915/9.918 | 27.061/28.206/28.832 | 122.542/124.617/125.577 | 0.078 | inactive |
| Native | on | off | 32.624 | 0.000 | 0.000 | 32.624 | 6.458/8.036/9.130 | 12.044/13.171/13.774 | 30.623/31.455/31.834 | 0.072 | inactive |
| Native | on | auto | 16.308 | 0.000 | 16.323 | 32.631 | 7.084/8.515/9.427 | 27.180/28.516/29.033 | 61.304/63.046/63.770 | 0.077 | inactive |
| Native | on | 2x | 16.312 | 0.000 | 16.320 | 32.632 | 7.058/8.540/9.148 | 27.235/28.688/29.042 | 61.298/63.197/63.950 | 0.076 | inactive |
| Native | on | 3x | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unsupported |
| Native | on | 4x | 8.161 | 0.000 | 24.482 | 32.642 | 7.486/9.084/10.010 | 27.104/28.530/29.050 | 122.524/124.633/125.863 | 0.079 | inactive |
| Performance | off | off | 32.588 | 0.000 | 0.000 | 32.588 | 6.829/8.406/9.209 | 11.974/13.156/13.542 | 30.699/31.474/31.963 | 0.070 | measured |
| Performance | off | auto | 32.608 | 0.000 | 0.000 | 32.608 | 6.757/8.281/9.247 | 11.937/13.148/13.627 | 30.622/31.516/31.964 | 0.071 | inactive |
| Performance | off | 2x | 32.588 | 0.000 | 0.000 | 32.588 | 6.784/8.425/9.421 | 11.975/13.183/13.709 | 30.651/31.505/31.941 | 0.070 | inactive |
| Performance | off | 3x | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unsupported |
| Performance | off | 4x | 32.610 | 0.000 | 0.000 | 32.610 | 6.789/8.330/9.340 | 11.943/13.108/13.505 | 30.642/31.450/31.817 | 0.070 | inactive |
| Performance | on | off | 32.609 | 0.000 | 0.000 | 32.609 | 7.029/8.658/9.511 | 12.208/13.321/13.835 | 30.642/31.488/31.857 | 0.071 | measured |
| Performance | on | auto | 32.617 | 0.000 | 0.000 | 32.617 | 7.020/8.440/9.318 | 12.152/13.313/13.728 | 30.635/31.491/31.991 | 0.070 | inactive |
| Performance | on | 2x | 32.586 | 0.000 | 0.000 | 32.586 | 6.968/8.583/9.481 | 12.152/13.343/13.761 | 30.648/31.492/31.871 | 0.071 | inactive |
| Performance | on | 3x | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unsupported |
| Performance | on | 4x | 32.598 | 0.000 | 0.000 | 32.598 | 6.988/8.541/9.278 | 12.155/13.397/13.854 | 30.646/31.500/31.857 | 0.071 | inactive |
| Balanced | off | off | 32.615 | 0.000 | 0.000 | 32.615 | 6.803/8.480/9.252 | 12.560/13.730/14.280 | 30.614/31.472/31.891 | 0.069 | measured |
| Balanced | off | auto | 32.589 | 0.000 | 0.000 | 32.589 | 6.787/8.365/9.269 | 12.634/13.833/14.347 | 30.675/31.485/31.898 | 0.070 | inactive |
| Balanced | off | 2x | 32.607 | 0.000 | 0.000 | 32.607 | 6.786/8.313/9.372 | 12.626/13.725/14.184 | 30.619/31.489/31.915 | 0.069 | inactive |
| Balanced | off | 3x | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unsupported |
| Balanced | off | 4x | 32.611 | 0.000 | 0.000 | 32.611 | 6.690/8.252/8.971 | 12.572/13.680/14.224 | 30.636/31.526/31.986 | 0.069 | inactive |
| Balanced | on | off | 32.609 | 0.000 | 0.000 | 32.609 | 6.991/8.546/9.482 | 12.754/13.925/14.360 | 30.653/31.459/31.819 | 0.069 | measured |
| Balanced | on | auto | 32.621 | 0.000 | 0.000 | 32.621 | 7.003/8.504/9.437 | 12.750/13.921/14.408 | 30.614/31.459/31.804 | 0.069 | inactive |
| Balanced | on | 2x | 32.608 | 0.000 | 0.000 | 32.608 | 6.883/8.392/9.233 | 12.747/13.972/14.357 | 30.627/31.480/31.838 | 0.069 | inactive |
| Balanced | on | 3x | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unsupported |
| Balanced | on | 4x | 32.607 | 0.000 | 0.000 | 32.607 | 6.916/8.453/9.149 | 12.729/13.875/14.398 | 30.646/31.489/31.971 | 0.070 | inactive |
| Quality | off | off | 32.599 | 0.000 | 0.000 | 32.599 | 6.824/8.352/9.548 | 13.883/15.041/15.627 | 30.653/31.526/32.007 | 0.070 | measured |
| Quality | off | auto | 32.592 | 0.000 | 0.000 | 32.592 | 6.826/8.365/9.146 | 13.903/15.094/15.548 | 30.657/31.499/31.866 | 0.070 | inactive |
| Quality | off | 2x | 32.602 | 0.000 | 0.000 | 32.602 | 6.847/8.451/9.152 | 13.863/14.951/15.489 | 30.612/31.498/31.953 | 0.070 | inactive |
| Quality | off | 3x | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unsupported |
| Quality | off | 4x | 32.597 | 0.000 | 0.000 | 32.597 | 6.781/8.360/8.949 | 13.863/15.039/15.444 | 30.635/31.505/31.884 | 0.072 | inactive |
| Quality | on | off | 32.603 | 0.000 | 0.000 | 32.603 | 7.059/8.575/9.483 | 14.031/15.222/15.674 | 30.659/31.504/31.901 | 0.070 | measured |
| Quality | on | auto | 32.612 | 0.000 | 0.000 | 32.612 | 6.978/8.561/9.306 | 14.016/15.221/15.664 | 30.648/31.454/31.858 | 0.069 | inactive |
| Quality | on | 2x | 32.590 | 0.000 | 0.000 | 32.590 | 7.001/8.489/9.476 | 14.057/15.252/15.723 | 30.668/31.517/31.879 | 0.070 | inactive |
| Quality | on | 3x | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unsupported |
| Quality | on | 4x | 32.594 | 0.000 | 0.000 | 32.594 | 7.000/8.624/9.416 | 14.053/15.177/15.703 | 30.644/31.530/31.917 | 0.070 | inactive |

## Source integrity

This table is rounded for reading. Exact counts, interval bounds, settings, qualification reasons and source hashes remain in the [structured results](results.json.gz) and [40-row CSV](rows.csv). Uncompressed results SHA256: `6a531bb01112835e5247b5181c57ee556b83fcfb85f7f71fdc2e7073eeffba4a`.
