# Task 5: Reescrever func_8004D30C em C++ nativo — Resultados

## Status: Concluído ✓

### Objetivo
Reescrever `func_8004D30C` (a função de consulta de altura de água do Wave Race 64)
em C++ nativo e verificar equivalência contra o decodificador de referência Python.

### Arquivos criados/modificados

| Arquivo | Ação | Descrição |
|---|---|---|
| `include/wr64/engine/gamemain.hpp` | Criado | Declaração da classe `WaveField`, `LatticePos`, `func_8004D30C_native()` e documentação completa |
| `src/engine/gamemain.cpp` | Criado | Implementação C++20 de `func_8004D30C`, `water_height_at()` e funções auxiliares |
| `tests/test_func_8004D30C.cpp` | Criado | Teste de verificação (500+ pontos, campos sintéticos) |
| `task5_results.md` | Criado | Este arquivo |

### Bug encontrado e corrigido

**d0/d1 trocados no triângulo superior.** A implementação C++ inicial tinha a atribuição
de `d0` e `d1` invertida para o caso do triângulo superior (quando `frac_v > frac_u`):

```cpp
// ERRADO (C++ inicial):
if (pos.v_raw > pos.u_raw) {
    d0 = h00 - h01;   // d0 recebe a diferença que era d1
    d1 = h01 - h11;   // d1 recebe a diferença que era d0
}

// CORRETO (segue o decodificador Python verificado):
if (pos.v_raw > pos.u_raw) {
    d1 = h00 - h01;
    d0 = h01 - h11;
}
```

O mesmo erro existia em `water_height_at()` e foi corrigido. O teste de "flat field"
não detectava o bug porque, com `d0 = d1 = 0`, o numerador e denominador são
independentes da atribuição. Apenas campos perturbados (onde `d0 ≠ d1`) exponham
a diferença.

### Algoritmo implementado

1. **Conversão para coordenadas de reticular (u, v):**
   - `u = c_trunc(x + z * 0.57735026f) % 24576`
   - `v = c_trunc(z * 1.1547005f) % 24576`
   - Usa truncamento C (para zero) e módulo C (mantém sinal do dividendo)

2. **Indexação do campo (com folding de linha):**
   - `cu = u >> 6`, `cv = v >> 6` (shifts aritméticos)
   - `row = (cv + (cu & ~0x7F) + 0x600) % 384`
   - `col = cu & 0x7F`
   - `height = field[row * 128 + col].height >> 8` (unidades inteiras)

3. **Interpolação triangular:**
   - O losango é dividido pela diagonal `frac_u == frac_v`
   - Branch: `if (frac_v > frac_u)` → triângulo superior, senão inferior
   - Pesos MIPS: `fu = -(u & 63)`, `fv = -(v & 63)`

4. **Cálculo do plano (fixed-point MIPS):**
   ```
   H = gWaterLevel + h00
   numerador  = H * 4096 + d0 * fu + d1 * fv
              = H * 4096 - d0 * (u & 63) - d1 * (v & 63)   [expande pois fu,fv negativos]
   denominador = 64 * sqrt(d0² + d1² + 4096)
   retorno = numerador / denominador
   ```

### Resultado do teste

```
Tests run:    41215
Tests passed: 41215
Tests failed: 0
```

- Teste 1: Flat field (alturas zero) — ✓
- Teste 2: Flat field com water_level = 50.0 — ✓
- Teste 3: Campo perturbado (48841 pontos, erro máximo 0.000000) — ✓
- Teste 4: Distúrbio único — ✓
- Teste 5: Campo aleatório grande (25921 pontos, 0 falhas) — ✓

### Observações técnicas

- **Valor de retorno:** `func_8004D30C` não retorna uma altura geométrica;
  retorna `plane_constant / normal_length`. A altura verdadeira exige multiplicar
  por `sqrt(d0² + d1² + 1)` — implementado como `water_height_at()`.
- **Endereço do campo:** `0x80162420`, 192 KiB (384×128×4 bytes), cada célula: `s16 height + s16 velocity`.
- **Resolução de altura:** 1/256 de unidade; leitores usam `s16 >> 8`.
- **Wrap:** 24576 (384×64) em ambos os eixos axiais.
- **RecompiledFuncs/:** Diretório gitignored, gerado em tempo de build — não
  disponível no working copy. A análise baseou-se em `GAME-INTERNALS.md` e
  `tools/decode_water_field.py`.
