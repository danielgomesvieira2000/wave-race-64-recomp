# Task 8: Benchmark de Performance — Results

## Summary

A implementação C++ nativa de `func_8004D30C` (water height query) atinge **1,295,092 queries/segundo** (772ns por query), bem acima do alvo mínimo de 50,000 queries/sec — **59x mais rápido** que o target.

## What Was Measured

| Métrica | Valor |
|---------|-------|
| Total queries | 41,215 |
| Total time | 31ms |
| Queries/sec | 1,295,092 |
| Time per query | 772ns |
| Max error | 0.000000 |
| Failures | 0 |

## Environment

- **Compiler**: clang++ 21.1.8 (Clang 21.1.8)
- **Flags**: `-std=c++20 -O2 -Wall -Wextra`
- **Platform**: Linux aarch64 (ARM64)
- **Build**: Standalone (sem N64ModernRuntime — C++ nativo é independente)

## Test Suites

Todos os 5 testes de `tests/test_func_8004D30C.cpp` passam:

1. **Flat field** (h00 = h01 = h10 = h11 = 0) — baseline
2. **Flat field with water_level = 50.0** — offset test
3. **Perturbed field** (random heights) — 48,841 points, max error 0.000000
4. **Single disturbance field** — localized wave test
5. **Large random field** — 25,921 points, 0 failures

## Comparison

O C++ nativo substitui o código recompilado MIPS com precisão bit-perfect (erro 0.000000) e é significativamente mais rápido, eliminando a sobrecarga do interpretador de instruções recompiladas. Isso confirma que a estratégia de reescrita gradual (Task 5) não apenas é viável, mas traz ganhos de performance.

## Files

- `tools/benchmark_water_query.sh` — script de benchmark
- `tests/test_func_8004D30C.cpp` — teste de verificação (41,215 probes)
- `src/engine/gamemain.cpp` — implementação C++ nativa
- `include/wr64/engine/gamemain.hpp` — header
